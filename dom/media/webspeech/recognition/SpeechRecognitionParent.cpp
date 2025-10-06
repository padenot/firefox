/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionParent.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

#include "mozIRemoteLazyInputStream.h"
#include "mozilla/Logging.h"
#include "mozilla/dom/Blob.h"
#include "mozilla/dom/BlobImpl.h"
#include "mozilla/dom/IPCBlobUtils.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/ipc/HWInferenceChild.h"
#include "mozilla/ipc/PHWInference.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/ipc/UtilityProcessChild.h"
#include "mozilla/llama/LlamaRuntimeLinker.h"
#include "nsDebug.h"
#include "nsGkAtoms.h"
#include "nsIFileStreams.h"
#include "nsNetUtil.h"
#include "nsString.h"
#include "prio.h"
#include "private/pprio.h"

#ifdef XP_WIN
#  include <fcntl.h>
#endif

namespace mozilla::ipc {

static LazyLogModule gSpeechRecognitionParentLog("SpeechRecognitionParent");
#define LOGV(fmt, ...)                                             \
  MOZ_LOG_FMT(gSpeechRecognitionParentLog, LogLevel::Verbose, fmt, \
              ##__VA_ARGS__)
#define LOGD(fmt, ...) \
  MOZ_LOG_FMT(gSpeechRecognitionParentLog, LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) \
  MOZ_LOG_FMT(gSpeechRecognitionParentLog, LogLevel::Error, fmt, ##__VA_ARGS__)

static constexpr int32_t DEFAULT_RECOGNITION_INTERVAL_MS = 1000;  // 1 second
static constexpr int32_t DEFAULT_AUDIO_LENGTH_MS =
    10000;  // 10 seconds of audio to analyze
static constexpr int32_t DEFAULT_NUM_THREADS = 4;

// Whisper sampling strategy enum value
static constexpr int WHISPER_SAMPLING_GREEDY = 0;

// Metadata callback for model blob file descriptor retrieval
class SpeechRecognitionMetadataCallback final : public nsIFileMetadataCallback {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  explicit SpeechRecognitionMetadataCallback(SpeechRecognitionParent* aParent)
      : mParent(aParent) {}

  NS_IMETHOD OnFileMetadataReady(nsIAsyncFileMetadata* aObject) override {
    if (mParent) {
      mParent->OnModelMetadataReceived();
    }
    return NS_OK;
  }

 private:
  virtual ~SpeechRecognitionMetadataCallback() = default;
  SpeechRecognitionParent* mParent = nullptr;
};

NS_IMPL_ISUPPORTS(SpeechRecognitionMetadataCallback, nsIFileMetadataCallback)

SpeechRecognitionParent::ModelIdentifier
SpeechRecognitionParent::LanguagesToModelIdentifier(
    const nsTArray<nsCString>& aLanguages) {
  MOZ_ASSERT(!aLanguages.IsEmpty());

  // Map languages to model names for speech recognition
  // en goes to ggml-small.en
  // all other languages fall back to large turbo v3
  nsCString modelName;
  nsCString fileName;
  nsCString revision = "main"_ns;

  // todo while loop, support requesting multiple languages
  const nsCString& firstLang = aLanguages[0];
  if (firstLang.EqualsLiteral("en") || firstLang.EqualsLiteral("en-US")) {
    modelName = "asr-test/whisper"_ns;
    fileName = "ggml-small.en.bin"_ns;
  } else {
    modelName = "asr-test/whisper"_ns;
    fileName = "ggml-large-v3-turbo-q8_0.bin"_ns;
  }

  return {modelName, fileName, revision};
}

nsCString SpeechRecognitionParent::ModelIdentifier::ToString() const {
  return nsFmtCString(FMT_STRING("{}/{}/{}"), mModelName.get(), mFileName.get(),
                      mRevision.get());
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvIsModelAvailable(
    const nsTArray<nsCString>& aLanguages,
    IsModelAvailableResolver&& aResolver) {
  LOGD("{} (id={}) RecvIsModelAvailable called for languages: {}", __func__,
       static_cast<unsigned long>(mSessionId), fmt::join(aLanguages, ", "));

  ModelIdentifier modelIdentifier = LanguagesToModelIdentifier(aLanguages);

  // Get HWInferenceChild to check model availability
  RefPtr<mozilla::ipc::UtilityProcessChild> utilityChild =
      mozilla::ipc::UtilityProcessChild::GetSingleton();
  if (!utilityChild) {
    LOGE("{} (id={}) No UtilityProcessChild available", __func__, mSessionId);
    aResolver(false);
    return IPC_OK();
  }

  mozilla::ipc::HWInferenceChild* hwInferenceChild =
      utilityChild->GetHWInferenceChild();
  if (!hwInferenceChild) {
    LOGE("{} (id={}) No HWInferenceChild available", __func__, mSessionId);
    aResolver(false);
    return IPC_OK();
  }

  LOGD(
      "{} (id={}) Sending model availability request to main process, {} "
      "mapped to model={}",
      __func__, mSessionId, fmt::join(aLanguages, ", "),
      modelIdentifier.ToString().get());

  auto promise = hwInferenceChild->SendIsModelAvailable(
      modelIdentifier.mModelName, modelIdentifier.mRevision,
      modelIdentifier.mFileName);
  promise->Then(
      GetCurrentSerialEventTarget(), __func__,
      [self = RefPtr{this}, resolver = aResolver](bool aAvailable) mutable {
        LOGD("Sending response back to content process: available={}",
             aAvailable ? "true" : "false");
        resolver(aAvailable);
      },
      [self = RefPtr{this},
       resolver = aResolver](ResponseRejectReason aReason) mutable {
        LOGE("{} IPC call to main process failed: {}", __func__,
             static_cast<int>(aReason));
        resolver(false);
      });

  return IPC_OK();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvInstallModels(
    const nsTArray<nsCString>& aLanguages, InstallModelsResolver&& aResolver) {
  ModelIdentifier modelIdentifier = LanguagesToModelIdentifier(aLanguages);

  LOGD("[{} (id={}) Mapped to model: {}, revision: {}, filename: {}", __func__,
       static_cast<unsigned long>(mSessionId), modelIdentifier.mModelName.get(),
       modelIdentifier.mRevision.get(), modelIdentifier.mFileName.get());

  RefPtr<mozilla::ipc::UtilityProcessChild> utilityChild =
      mozilla::ipc::UtilityProcessChild::GetSingleton();
  if (!utilityChild) {
    LOGE("{} No UtilityProcessChild available", __func__,
         static_cast<unsigned long>(mSessionId));
    aResolver(false);
    return IPC_OK();
  }

  mozilla::ipc::HWInferenceChild* hwInferenceChild =
      utilityChild->GetHWInferenceChild();
  if (!hwInferenceChild) {
    LOGE("{} No HWInferenceChild available", __func__,
         static_cast<unsigned long>(mSessionId));
    aResolver(false);
    return IPC_OK();
  }

  LOGD(
      "{} Sending model installation request to main process via "
      "HWInference: model={}",
      __func__, static_cast<unsigned long>(mSessionId),
      modelIdentifier.ToString().get());

  hwInferenceChild
      ->SendInstallModel(modelIdentifier.mModelName, modelIdentifier.mRevision,
                         modelIdentifier.mFileName)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr(this), aResolver](bool aSuccess) mutable {
            LOGD(
                "{} Received installation response from main process: "
                "success={}",
                __func__, aSuccess ? "true" : "false");
            aResolver(aSuccess);
          },
          [self = RefPtr(this),
           aResolver](ResponseRejectReason aReason) mutable {
            LOGE("{} IPC call to main process failed: {}", __func__,
                 static_cast<int>(aReason));
            aResolver(false);
          });

  return IPC_OK();
}

SpeechRecognitionParent::SpeechRecognitionParent(uint64_t aSessionId)
    : mSessionId(aSessionId),
      mIsActive(false),
      mWhisperCtx(nullptr),
      mLib(nullptr),
      mThreadRunning(false),
      mAudioQueue(WHISPER_SAMPLE_RATE * 30),  // 30 seconds of audio buffer
      mRingWritePos(0),
      mRingSize(WHISPER_SAMPLE_RATE *
                (DEFAULT_AUDIO_LENGTH_MS / 1000)),  // 10 seconds
      mRecognitionIntervalMs(DEFAULT_RECOGNITION_INTERVAL_MS),
      mAudioLengthMs(DEFAULT_AUDIO_LENGTH_MS),
      mNumThreads(DEFAULT_NUM_THREADS) {
  // Initialize ring buffer
  mAudioRing.resize(mRingSize, 0.0f);

  // MOZ_DUMP_AUDIO=1 MOZ_DISABLE_UTILITY_SANDBOX=1 to activate this
  mWhisperAudioDumper.Open("SpeechRecognition-Whisper-Input", 1,
                           WHISPER_SAMPLE_RATE);
}

void SpeechRecognitionParent::RetrieveModelBlob() {
  RefPtr<mozilla::ipc::UtilityProcessChild> utilityChild =
      mozilla::ipc::UtilityProcessChild::GetSingleton();
  if (!utilityChild) {
    LOGE("{} (id={}) ERROR: No UtilityProcessChild available", __func__,
         mSessionId);
    return;
  }
  mozilla::ipc::HWInferenceChild* hwInferenceChild =
      utilityChild->GetHWInferenceChild();
  if (!hwInferenceChild) {
    LOGE("{} (id={}) No HWInferenceChild available for model blob retrieval",
         __func__, mSessionId);
    return;
  }

  ModelIdentifier modelIdentifier =
      LanguagesToModelIdentifier(nsTArray{mLanguage});

  LOGD("{} (id={}) Requesting model blob: model={}", __func__, mSessionId,
       modelIdentifier.ToString().get());

  hwInferenceChild
      ->SendGetModelBlob(modelIdentifier.mModelName, modelIdentifier.mRevision,
                         modelIdentifier.mFileName)
      ->Then(
          GetCurrentSerialEventTarget(), __func__,
          [self = RefPtr{this}](
              const mozilla::ipc::GetModelBlobResult& aResult) mutable {
            if (aResult.type() ==
                mozilla::ipc::GetModelBlobResult::TGetModelBlobError) {
              LOGE("{} (id={}) GetModelBlobError with nsresult={:x}", __func__,
                   self->mSessionId,
                   static_cast<uint32_t>(
                       aResult.get_GetModelBlobError().errorCode()));
              return;
            }

            const mozilla::dom::IPCBlob& blob =
                aResult.get_GetModelBlobSuccess().blob();
            RefPtr<mozilla::dom::BlobImpl> blobImpl =
                mozilla::dom::IPCBlobUtils::Deserialize(blob);

            if (!blobImpl) {
              LOGE("{} (id={}): Could not deserialize IPCBlob", __func__,
                   self->mSessionId);
              return;
            }
            mozilla::ErrorResult errorResult;
            blobImpl->CreateInputStream(getter_AddRefs(self->mModelStream),
                                        errorResult);
            if (errorResult.Failed()) {
              LOGE("{} (id={}): CreateInputStream failed", __func__,
                   self->mSessionId);
              return;
            }

            nsCOMPtr<nsIEventTarget> eventTarget =
                mozilla::GetCurrentSerialEventTarget();
            nsCOMPtr<nsIAsyncFileMetadata> asyncFileMetadata =
                do_QueryInterface(self->mModelStream);
            MOZ_ASSERT(asyncFileMetadata,
                       "Programming error: Stream does not support "
                       "nsIAsyncFileMetadata interface");

            self->mMetadataCallback =
                MakeAndAddRef<SpeechRecognitionMetadataCallback>(self);
            nsresult rv = asyncFileMetadata->AsyncFileMetadataWait(
                self->mMetadataCallback.get(), eventTarget);
            if (NS_WARN_IF(NS_FAILED(rv))) {
              LOGE("{} (id={})AsyncFileMetadataWait returned error 0x{:x}",
                   __func__, self->mSessionId, static_cast<uint32_t>(rv));
              return;
            }
          },
          [self = RefPtr{this}](
              mozilla::ipc::ResponseRejectReason aReason) mutable {
            LOGE("{} (id={}) Promise rejected with reason {}", __func__,
                 self->mSessionId, static_cast<int>(aReason));
          });
}

void SpeechRecognitionParent::OnModelMetadataReceived() {
  mMetadataCallback = nullptr;

  const nsCOMPtr<nsIFileMetadata> fileMetadata =
      do_QueryInterface(mModelStream);
  MOZ_ASSERT(
      fileMetadata,
      "Programming error, mModelStream doesn't implement nsIFileMetadata");

  // Get FILE* from mModelStream
  PRFileDesc* fileDesc;
  const nsresult rv = fileMetadata->GetFileDescriptor(&fileDesc);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    LOGE("{} (id={}) GetFileDescriptor error, neresult=0x{:x}", __func__,
         mSessionId, static_cast<uint32_t>(rv));
    return;
  }
  MOZ_ASSERT(fileDesc);
#ifdef XP_WIN
  // Convert our file descriptor to FILE*
  void* handle = mozilla::ipc::FileDescriptor::PlatformHandleType(
      PR_FileDesc2NativeHandle(fileDesc));
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY);
  if (fd == -1) {
    LOGE("{} (id={}): _open_osfhandle failed", __func__, mSessionId);
    return;
  }
#else
  PROsfd fd = PR_FileDesc2NativeHandle(fileDesc);
#endif
  FILE* fp = fdopen(fd, "rb");
  if (!fp) {
    LOGE("{} (id={}) fdopen failed", __func__, mSessionId);
    return;
  }

  mModelFile = fp;
  mWhisperInitPending.store(true);
}

SpeechRecognitionParent::~SpeechRecognitionParent() {
  LOGD("{} (id={}) mIsActive={}", __func__, mSessionId,
       mIsActive ? "true" : "false");
  if (mIsActive) {
    LOGD("[SRParent:{}] Destroying active session, cleaning up",
         static_cast<unsigned long>(mSessionId));
    mIsActive = false;
  }

  if (mThreadRunning.load()) {
    mThreadRunning.store(false);
    if (mBackgroundThread.joinable()) {
      mBackgroundThread.join();
    }
  }

  CleanupWhisperContext();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvInit(
    const nsCString& aLanguage, const nsTArray<nsString>& aPhrases,
    InitResolver&& aResolver) {
  LOGD("{} (id={}) language='{}'", __func__, mSessionId, aLanguage.get());

  mLanguage = aLanguage;
  mPhrases = aPhrases.Clone();
  mIsActive = true;

  if (!mThreadRunning.load()) {
    mThreadRunning.store(true);
    mBackgroundThread = std::thread([self = RefPtr{this}]() {
      self->InitializeWhisperOnBackgroundThread();
      self->ProcessAudioOnBackgroundThread();
    });
  }

  // Since we're using blob-based loading, we'll initialize in the background
  // thread For now, just resolve success
  aResolver(true);
  return IPC_OK();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvProcessAudioData(
    nsTArray<float>&& aAudioData) {
  LOGV("{} (id={}) {} samples", __func__, mSessionId, aAudioData.Length());

  if (!mIsActive) {
    LOGD(
        "[SRParent:{}] Received audio data but session is not active, ignoring",
        static_cast<unsigned long>(mSessionId));
    return IPC_OK();
  }

  if (!mAudioQueue.Enqueue(aAudioData.Elements(), (int)aAudioData.Length())) {
    LOGD("[SRParent:{}] Audio queue full, dropping sample",
         static_cast<unsigned long>(mSessionId));
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvStop() {
  LOGD("[SRParent:{}] RecvStop called, mIsActive={}",
       static_cast<unsigned long>(mSessionId), mIsActive ? "true" : "false");

  if (mIsActive) {
    mIsActive = false;

    // Stop background thread
    if (mThreadRunning.load()) {
      mThreadRunning.store(false);
      LOGD("[SRParent:{}] Signaled background thread to stop",
           static_cast<unsigned long>(mSessionId));
    }

    LOGD(
        "[SRParent:{}] Stopping speech recognition session and cleaning up "
        "resources",
        static_cast<unsigned long>(mSessionId));
  } else {
    LOGD("[SRParent:{}] Stop called on inactive session",
         static_cast<unsigned long>(mSessionId));
  }
  return IPC_OK();
}

void SpeechRecognitionParent::ActorDestroy(ActorDestroyReason aReason) {
  LOGD("[SRParent:{}] ActorDestroy called, reason={}, mIsActive={}",
       static_cast<unsigned long>(mSessionId), static_cast<int>(aReason),
       mIsActive ? "true" : "false");

  if (mIsActive) {
    LOGD("[SRParent:{}] Actor destroyed while session was active, cleaning up",
         static_cast<unsigned long>(mSessionId));
    mIsActive = false;
  }

  // Stop background thread
  if (mThreadRunning.load()) {
    mThreadRunning.store(false);
    if (mBackgroundThread.joinable()) {
      mBackgroundThread.join();
    }
  }
}

void SpeechRecognitionParent::InitializeWhisperOnBackgroundThread() {
  if (!mLib) {
    mLib = mozilla::llama::LlamaRuntimeLinker::Get();
    if (!mLib) {
      LOGE("[SRParent:{}] Failed to get runtime linker",
           static_cast<unsigned long>(mSessionId));
      return;
    }
  }

  RefPtr<SpeechRecognitionParent> self = this;
  NS_DispatchToMainThread(
      NS_NewRunnableFunction("SpeechRecognitionParent::RetrieveModelBlob",
                             [self]() { self->RetrieveModelBlob(); }));
}

void SpeechRecognitionParent::ProcessAudioOnBackgroundThread() {
  LOGD("{} (id={}) Starting recognition loop", __func__,
       static_cast<unsigned long>(mSessionId));

  std::vector<float> audioForRecognition;

  // Two background tasks: audio ring buffer management and periodic recognition
  std::thread audioConsumerThread([self = RefPtr{this}]() {
    while (self->mThreadRunning.load()) {
      // Continuously dequeue audio and add to ring buffer
      size_t available = self->mAudioQueue.AvailableRead();
      if (available > 0) {
        // Process available audio in chunks
        size_t samples_to_process =
            std::min(available, static_cast<size_t>(1024));
        std::vector<float> samples(samples_to_process);
        size_t dequeued =
            self->mAudioQueue.Dequeue(samples.data(), samples_to_process);

        // Add samples to ring buffer
        for (size_t i = 0; i < dequeued; i++) {
          self->mAudioRing[self->mRingWritePos] = samples[i];
          self->mRingWritePos = (self->mRingWritePos + 1) % self->mRingSize;
        }

        LOGV("[SRParent:{}] Added {} samples to ring buffer, writePos={}",
             static_cast<unsigned long>(self->mSessionId), dequeued,
             self->mRingWritePos);
      }

      // Small sleep to prevent excessive CPU usage
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  // Main recognition loop - runs every recognition interval
  auto lastRecognitionTime = std::chrono::steady_clock::now();

  while (mThreadRunning.load()) {
    // Check if we need to initialize Whisper with the FILE* handle
    if (mWhisperInitPending.load() && mModelFile && !mWhisperCtx) {
      struct whisper_context_params cparams =
          mLib->whisper_context_default_params();

#ifdef XP_MACOSX
      cparams.use_gpu = true;
#else
      cparams.use_gpu = false;
#endif

      mWhisperCtx =
          mLib->whisper_init_from_file_handle_with_params(mModelFile, cparams);

      if (!mWhisperCtx) {
        LOGE(
            "{} (id={}) ERROR whisper_init_from_file_handle_with_params "
            "returned nullptr",
            __func__, mSessionId);
        fclose(mModelFile);
        mModelFile = nullptr;
      } else {
        LOGD("{} (id={}) Got Whisper context", __func__, mSessionId);
      }

      mWhisperInitPending.store(false);
    }

    auto currentTime = std::chrono::steady_clock::now();
    auto timeSinceLastRecognition =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            currentTime - lastRecognitionTime)
            .count();

    if (timeSinceLastRecognition >= mRecognitionIntervalMs) {
      // Time for recognition - extract audio from ring buffer
      size_t samples_to_analyze = std::min(
          mRingSize,
          static_cast<size_t>((1e-3 * mAudioLengthMs) * WHISPER_SAMPLE_RATE));

      audioForRecognition.resize(samples_to_analyze);

      // Extract from ring buffer (most recent samples)
      for (size_t i = 0; i < samples_to_analyze; i++) {
        size_t ring_pos =
            (mRingWritePos + mRingSize - samples_to_analyze + i) % mRingSize;
        audioForRecognition[i] = mAudioRing[ring_pos];
      }

      LOGD("[SRParent:{}] Running recognition on {} samples ({:.2f}s of audio)",
           static_cast<unsigned long>(mSessionId), samples_to_analyze,
           samples_to_analyze / (float)WHISPER_SAMPLE_RATE);

      // Dump audio data that will be sent to Whisper for debugging
      mWhisperAudioDumper.Write(audioForRecognition.data(),
                                audioForRecognition.size());

      if (mWhisperCtx && audioForRecognition.size() > 0 && mLib) {
        // Run Whisper inference
        whisper_full_params wparams = mLib->whisper_full_default_params(
            whisper_sampling_strategy(WHISPER_SAMPLING_GREEDY));
        wparams.print_progress = false;
        wparams.print_special = false;
        wparams.print_realtime = false;
        wparams.print_timestamps = true;
        wparams.translate = false;
        wparams.single_segment = false;
        wparams.max_tokens = 32;
        wparams.language = mLanguage.get();
        wparams.n_threads = mNumThreads;
        wparams.audio_ctx = 0;
        nsCString prompt;
        for (auto& phrase : mPhrases) {
          prompt.Append(NS_ConvertUTF16toUTF8(phrase));
          prompt.AppendLiteral(". ");
        }
        wparams.initial_prompt = prompt.get();

        if (mLib->whisper_full(mWhisperCtx, wparams, audioForRecognition.data(),
                               audioForRecognition.size()) == 0) {
          // Process results
          const int n_segments = mLib->whisper_full_n_segments(mWhisperCtx);

          for (int i = 0; i < n_segments; ++i) {
            const char* text =
                mLib->whisper_full_get_segment_text(mWhisperCtx, i);

            if (text && strlen(text) > 0) {
              nsCString transcript(text);
              bool isFinal =
                  (i == n_segments - 1);  // Mark last segment as final

              LOGV("{} recognition result: '{}' (final={})",
                   __func__, mSessionId, transcript.get(), isFinal ? "true" : "false");

              // Send result via IPC (dispatch to main thread)
              NS_DispatchToMainThread(NS_NewRunnableFunction(
                  "SpeechRecognitionParent::SendResult",
                  [self = RefPtr{this}, transcript = nsCString(transcript),
                   isFinal]() {
                    if (self->CanSend()) {
                      Unused
                          << self->SendOnRecognitionResult(transcript, isFinal);
                    }
                  }));
            }
          }
        } else {
          LOGD("[SRParent:{}] Whisper inference failed",
               static_cast<unsigned long>(mSessionId));
        }
      }

      lastRecognitionTime = currentTime;
    }

    // Sleep until next recognition time
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  // Wait for audio consumer thread to finish
  if (audioConsumerThread.joinable()) {
    audioConsumerThread.join();
  }

  LOGD("[SRParent:{}] Continuous audio processing loop terminated",
       static_cast<unsigned long>(mSessionId));
}

void SpeechRecognitionParent::CleanupWhisperContext() {
  if (mWhisperCtx && mLib) {
    mLib->whisper_free(mWhisperCtx);
    mWhisperCtx = nullptr;
  }

  if (mModelFile) {
    fclose(mModelFile);
    mModelFile = nullptr;
  }
}

}  // namespace mozilla::ipc

#undef LOGV
#undef LOGD
#undef LOGE
