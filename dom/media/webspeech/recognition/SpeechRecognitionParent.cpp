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

#include "mozilla/Logging.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/ipc/UtilityProcessChild.h"
#include "mozilla/ipc/HWInferenceChild.h"
#include "mozilla/ipc/PHWInference.h"
#include "mozilla/dom/IPCBlobUtils.h"
#include "mozilla/dom/BlobImpl.h"
#include "mozilla/llama/LlamaRuntimeLinker.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/Blob.h"
#include "nsIFileStreams.h"
#include "nsDebug.h"
#include "mozIRemoteLazyInputStream.h"
#include "nsNetUtil.h"
#include "prio.h"
#include "private/pprio.h"

#ifdef XP_WIN
#  include <fcntl.h>
#endif

namespace mozilla::ipc {

static LazyLogModule gSpeechRecognitionParentLog("SpeechRecognitionParent");
#define SR_LOGD(fmt, ...) \
  MOZ_LOG_FMT(gSpeechRecognitionParentLog, LogLevel::Debug, fmt, ##__VA_ARGS__)

#define SR_LOGE(fmt, ...) \
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

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvIsModelAvailable(
    const nsTArray<nsCString>& aLanguages,
    IsModelAvailableResolver&& aResolver) {
  SR_LOGD("[SRParent:{}] RecvIsModelAvailable called for {} languages",
       static_cast<unsigned long>(mSessionId), aLanguages.Length());

  MOZ_ASSERT(!aLanguages.IsEmpty());

  for (const auto& lang : aLanguages) {
    SR_LOGD("[SRParent:{}]   - Language: {}", static_cast<unsigned long>(mSessionId), lang.get());
  }

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

  SR_LOGD("[SRParent:{}] Mapped to model: {}, revision: {}",
       static_cast<unsigned long>(mSessionId), modelName.get(), revision.get());

  // Get HWInferenceChild to check model availability
  RefPtr<mozilla::ipc::UtilityProcessChild> utilityChild = mozilla::ipc::UtilityProcessChild::GetSingleton();
  if (!utilityChild) {
    SR_LOGE("[SRParent:{}] No UtilityProcessChild available", static_cast<unsigned long>(mSessionId));
    aResolver(false);
    return IPC_OK();
  }

  mozilla::ipc::HWInferenceChild* hwInferenceChild = utilityChild->GetHWInferenceChild();
  if (!hwInferenceChild) {
    SR_LOGE("[SRParent:{}] No HWInferenceChild available", static_cast<unsigned long>(mSessionId));
    aResolver(false);
    return IPC_OK();
  }

  SR_LOGD("[SRParent:{}] Sending model availability request to main process via HWInference: "
       "model={} revision={} filename={}",
       static_cast<unsigned long>(mSessionId), modelName.get(), revision.get(), fileName.get());

  // Send request to main process via HWInferenceChild
  auto sharedResolver =
      std::make_shared<IsModelAvailableResolver>(std::move(aResolver));
  auto promise = hwInferenceChild->SendIsModelAvailable(modelName, revision, fileName);
  promise->Then(
      GetCurrentSerialEventTarget(), __func__,
      [self = RefPtr(this), sharedResolver, this](bool aAvailable) mutable {
        SR_LOGD("[SRParent:{}] Sending response back to child process: available={}",
                 static_cast<unsigned long>(mSessionId), aAvailable ? "true" : "false");
        (*sharedResolver)(aAvailable);
      },
      [self = RefPtr(this), sharedResolver, this](ResponseRejectReason aReason) mutable {
        SR_LOGE("[SRParent:{}] IPC call to main process failed: {}",
                 static_cast<unsigned long>(mSessionId), static_cast<int>(aReason));
        (*sharedResolver)(false);
      });

  return IPC_OK();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvInstallModels(
    const nsTArray<nsCString>& aLanguages, InstallModelsResolver&& aResolver) {
  SR_LOGD("[SRParent:{}] RecvInstallModels called for {} languages",
       static_cast<unsigned long>(mSessionId), aLanguages.Length());

  for (const auto& lang : aLanguages) {
    SR_LOGD("[SRParent:{}]   - Language to install: {}", static_cast<unsigned long>(mSessionId), lang.get());
  }

  // Map languages to model names for speech recognition
  // en goes to ggml-small.en
  // all other languages fall back to large turbo v3
  nsCString modelName;
  nsCString fileName;
  nsCString revision = "main"_ns;

  // TODO: Handle multiple languages properly in a loop
  const nsCString& firstLang = aLanguages[0];
  if (firstLang.EqualsLiteral("en") || firstLang.EqualsLiteral("en-US")) {
    modelName = "asr-test/whisper"_ns;
    fileName = "ggml-small.en.bin"_ns;
  } else {
    modelName = "asr-test/whisper"_ns;
    fileName = "ggml-large-v3-turbo-q8_0.bin"_ns;
  }

  SR_LOGD("[SRParent:{}] Mapped to model: {}, revision: {}, filename: {}",
       static_cast<unsigned long>(mSessionId), modelName.get(), revision.get(), fileName.get());

  // Get HWInferenceChild to start model download
  RefPtr<mozilla::ipc::UtilityProcessChild> utilityChild = mozilla::ipc::UtilityProcessChild::GetSingleton();
  if (!utilityChild) {
    SR_LOGE("[SRParent:{}] No UtilityProcessChild available", static_cast<unsigned long>(mSessionId));
    aResolver(false);
    return IPC_OK();
  }

  mozilla::ipc::HWInferenceChild* hwInferenceChild = utilityChild->GetHWInferenceChild();
  if (!hwInferenceChild) {
    SR_LOGE("[SRParent:{}] No HWInferenceChild available", static_cast<unsigned long>(mSessionId));
    aResolver(false);
    return IPC_OK();
  }

  SR_LOGD("[SRParent:{}] Sending model installation request to main process via HWInference: "
       "model={} revision={} filename={}",
       static_cast<unsigned long>(mSessionId), modelName.get(), revision.get(), fileName.get());

  // Send install request to main process via HWInferenceChild
  auto sharedResolver =
      std::make_shared<InstallModelsResolver>(std::move(aResolver));
  auto promise = hwInferenceChild->SendInstallModel(modelName, revision, fileName);
  promise->Then(
      GetCurrentSerialEventTarget(), __func__,
      [self = RefPtr(this), sharedResolver, this](bool aSuccess) mutable {
        SR_LOGD("[SRParent:{}] Received installation response from main process: success={}",
                 static_cast<unsigned long>(mSessionId), aSuccess ? "true" : "false");
        SR_LOGD("[SRParent:{}] Sending response back to child process", static_cast<unsigned long>(mSessionId));
        (*sharedResolver)(aSuccess);
      },
      [self = RefPtr(this), sharedResolver,
       this](ResponseRejectReason aReason) mutable {
        SR_LOGE("[SRParent:{}] IPC call to main process failed: {}",
                 static_cast<unsigned long>(mSessionId), static_cast<int>(aReason));
        (*sharedResolver)(false);
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

  // Initialize audio dumpers with descriptive names
  mIPCAudioDumper.Open("SpeechRecognition-IPC-Input", 1, WHISPER_SAMPLE_RATE);
  mWhisperAudioDumper.Open("SpeechRecognition-Whisper-Input", 1,
                           WHISPER_SAMPLE_RATE);

  SR_LOGD("[SRParent:{}] Constructor called", static_cast<unsigned long>(mSessionId));
}

void SpeechRecognitionParent::RetrieveModelBlob() {
  SR_LOGD("[SRParent:{}] === START RetrieveModelBlob ===", static_cast<unsigned long>(mSessionId));

  // Get HWInferenceChild from UtilityProcessChild
  SR_LOGD("[SRParent:{}] Step 1: Getting UtilityProcessChild singleton", static_cast<unsigned long>(mSessionId));
  RefPtr<mozilla::ipc::UtilityProcessChild> utilityChild = mozilla::ipc::UtilityProcessChild::GetSingleton();
  if (!utilityChild) {
    SR_LOGE("[SRParent:{}] FAILED: No UtilityProcessChild available", static_cast<unsigned long>(mSessionId));
    return;
  }
  SR_LOGD("[SRParent:{}] Step 1 SUCCESS: Got UtilityProcessChild", static_cast<unsigned long>(mSessionId));

  SR_LOGD("[SRParent:{}] Step 2: Getting HWInferenceChild from UtilityProcessChild", static_cast<unsigned long>(mSessionId));
  mozilla::ipc::HWInferenceChild* hwInferenceChild = utilityChild->GetHWInferenceChild();
  if (!hwInferenceChild) {
    SR_LOGE("[SRParent:{}] FAILED: No HWInferenceChild available for model blob retrieval", static_cast<unsigned long>(mSessionId));
    return;
  }
  SR_LOGD("[SRParent:{}] Step 2 SUCCESS: Got HWInferenceChild", static_cast<unsigned long>(mSessionId));

  // Determine model info based on language (same logic as RecvIsModelAvailable)
  nsCString modelName = "asr-test/whisper"_ns;
  nsCString revision = "main"_ns;
  nsCString fileName;

  if (mLanguage.EqualsLiteral("en") || mLanguage.EqualsLiteral("en-US")) {
    fileName = "ggml-small.en.bin"_ns;
  } else {
    fileName = "ggml-large-v3-turbo-q8_0.bin"_ns;
  }

  SR_LOGD("[SRParent:{}] Step 3: Requesting model blob: model={} revision={} file={}",
       static_cast<unsigned long>(mSessionId), modelName.get(), revision.get(), fileName.get());

  // Request model blob from parent process via HWInferenceChild
  SR_LOGD("[SRParent:{}] Step 4: Calling SendGetModelBlob IPC", static_cast<unsigned long>(mSessionId));
  auto promise = hwInferenceChild->SendGetModelBlob(modelName, revision, fileName);
  SR_LOGD("[SRParent:{}] Step 4 SUCCESS: SendGetModelBlob IPC call made", static_cast<unsigned long>(mSessionId));
  promise->Then(
      GetCurrentSerialEventTarget(), __func__,
      [self = RefPtr(this), this](const mozilla::ipc::GetModelBlobResult& aResult) mutable {
        SR_LOGD("[SRParent:{}] Step 5: Promise resolved, processing result", static_cast<unsigned long>(mSessionId));

        if (aResult.type() == mozilla::ipc::GetModelBlobResult::TGetModelBlobError) {
          SR_LOGE("[SRParent:{}] Step 5 FAILED: GetModelBlobError with code {}",
                  static_cast<unsigned long>(mSessionId),
                  static_cast<uint32_t>(aResult.get_GetModelBlobError().errorCode()));
          return;
        }

        SR_LOGD("[SRParent:{}] Step 5 SUCCESS: Got GetModelBlobSuccess result", static_cast<unsigned long>(mSessionId));

        // Get the IPCBlob from the success result
        SR_LOGD("[SRParent:{}] Step 6: Extracting IPCBlob from success result", static_cast<unsigned long>(mSessionId));
        const mozilla::dom::IPCBlob& blob = aResult.get_GetModelBlobSuccess().blob();
        SR_LOGD("[SRParent:{}] Step 6 SUCCESS: Got IPCBlob (size: {})",
                static_cast<unsigned long>(mSessionId), blob.size());

        // Convert IPCBlob to DOM Blob and create input stream
        SR_LOGD("[SRParent:{}] Step 7: Deserializing IPCBlob to BlobImpl", static_cast<unsigned long>(mSessionId));
        RefPtr<mozilla::dom::BlobImpl> blobImpl =
            mozilla::dom::IPCBlobUtils::Deserialize(blob);

        if (!blobImpl) {
          SR_LOGE("[SRParent:{}] Step 7 FAILED: Could not deserialize IPCBlob", static_cast<unsigned long>(mSessionId));
          return;
        }
        mozilla::ErrorResult sizeErr;
        uint64_t blobSize = blobImpl->GetSize(sizeErr);
        if (sizeErr.Failed()) {
          blobSize = 0;
        }
        SR_LOGD("[SRParent:{}] Step 7 SUCCESS: Deserialized to BlobImpl (size: {})",
                static_cast<unsigned long>(mSessionId), blobSize);

        SR_LOGD("[SRParent:{}] Step 8: Creating input stream from BlobImpl", static_cast<unsigned long>(mSessionId));
        mozilla::ErrorResult errorResult;
        blobImpl->CreateInputStream(getter_AddRefs(mModelStream), errorResult);
        if (errorResult.Failed()) {
          SR_LOGE("[SRParent:{}] Step 8 FAILED: CreateInputStream failed", static_cast<unsigned long>(mSessionId));
          return;
        }
        SR_LOGD("[SRParent:{}] Step 8 SUCCESS: Created input stream", static_cast<unsigned long>(mSessionId));

        // Check if stream supports file metadata interface
        SR_LOGD("[SRParent:{}] Step 9: Querying for nsIFileMetadata interface", static_cast<unsigned long>(mSessionId));
        nsCOMPtr<nsIFileMetadata> fileMetadata = do_QueryInterface(mModelStream);
        if (!fileMetadata) {
          SR_LOGE("[SRParent:{}] Step 9 FAILED: Stream does not support nsIFileMetadata interface", static_cast<unsigned long>(mSessionId));
          return;
        }
        SR_LOGD("[SRParent:{}] Step 9 SUCCESS: Got nsIFileMetadata interface", static_cast<unsigned long>(mSessionId));

        // Set up async metadata retrieval
        SR_LOGD("[SRParent:{}] Step 10: Setting up async metadata retrieval", static_cast<unsigned long>(mSessionId));
        nsCOMPtr<nsIEventTarget> eventTarget = mozilla::GetCurrentSerialEventTarget();

        SR_LOGD("[SRParent:{}] Step 11: Querying for nsIAsyncFileMetadata interface", static_cast<unsigned long>(mSessionId));
        nsCOMPtr<nsIAsyncFileMetadata> asyncFileMetadata = do_QueryInterface(mModelStream);
        if (!asyncFileMetadata) {
          SR_LOGE("[SRParent:{}] Step 11 FAILED: Stream does not support nsIAsyncFileMetadata", static_cast<unsigned long>(mSessionId));
          return;
        }
        SR_LOGD("[SRParent:{}] Step 11 SUCCESS: Got nsIAsyncFileMetadata interface", static_cast<unsigned long>(mSessionId));

        SR_LOGD("[SRParent:{}] Step 12: Creating metadata callback", static_cast<unsigned long>(mSessionId));
        mMetadataCallback = MakeAndAddRef<SpeechRecognitionMetadataCallback>(this);
        SR_LOGD("[SRParent:{}] Step 12 SUCCESS: Created callback", static_cast<unsigned long>(mSessionId));

        SR_LOGD("[SRParent:{}] Step 13: Calling AsyncFileMetadataWait", static_cast<unsigned long>(mSessionId));
        nsresult rv = asyncFileMetadata->AsyncFileMetadataWait(mMetadataCallback.get(), eventTarget);
        if (NS_WARN_IF(NS_FAILED(rv))) {
          SR_LOGE("[SRParent:{}] Step 13 FAILED: AsyncFileMetadataWait returned error 0x{:x}",
                  static_cast<unsigned long>(mSessionId), static_cast<uint32_t>(rv));
          return;
        }
        SR_LOGD("[SRParent:{}] Step 13 SUCCESS: AsyncFileMetadataWait called successfully", static_cast<unsigned long>(mSessionId));

        SR_LOGD("[SRParent:{}] === END RetrieveModelBlob (waiting for callback) ===", static_cast<unsigned long>(mSessionId));
      },
      [self = RefPtr(this), this](mozilla::ipc::ResponseRejectReason aReason) mutable {
        SR_LOGE("[SRParent:{}] Step 5 FAILED: Promise rejected with reason {} (0=SendError, 1=ChannelClosed, 2=HandlerRejected, 3=ActorDestroyed, 4=ResolverDestroyed)",
                 static_cast<unsigned long>(mSessionId), static_cast<int>(aReason));
        SR_LOGD("[SRParent:{}] === END RetrieveModelBlob (failed) ===", static_cast<unsigned long>(mSessionId));
      });
}

void SpeechRecognitionParent::OnModelMetadataReceived() {
  SR_LOGD("[SRParent:{}] === START OnModelMetadataReceived ===", static_cast<unsigned long>(mSessionId));
  SR_LOGD("[SRParent:{}] Step 14: Metadata callback invoked", static_cast<unsigned long>(mSessionId));

  mMetadataCallback = nullptr;

  SR_LOGD("[SRParent:{}] Step 15: Querying stream for nsIFileMetadata again", static_cast<unsigned long>(mSessionId));
  const nsCOMPtr<nsIFileMetadata> fileMetadata = do_QueryInterface(mModelStream);
  if (NS_WARN_IF(!fileMetadata)) {
    SR_LOGE("[SRParent:{}] Step 15 FAILED: QI for nsIFileMetadata failed", static_cast<unsigned long>(mSessionId));
    return;
  }
  SR_LOGD("[SRParent:{}] Step 15 SUCCESS: Got nsIFileMetadata", static_cast<unsigned long>(mSessionId));

  SR_LOGD("[SRParent:{}] Step 16: Getting file descriptor from metadata", static_cast<unsigned long>(mSessionId));
  PRFileDesc* fileDesc;
  const nsresult rv = fileMetadata->GetFileDescriptor(&fileDesc);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    SR_LOGE("[SRParent:{}] Step 16 FAILED: GetFileDescriptor returned error 0x{:x}",
            static_cast<unsigned long>(mSessionId), static_cast<uint32_t>(rv));
    return;
  }
  if (!fileDesc) {
    SR_LOGE("[SRParent:{}] Step 16 FAILED: GetFileDescriptor returned nullptr", static_cast<unsigned long>(mSessionId));
    return;
  }
  SR_LOGD("[SRParent:{}] Step 16 SUCCESS: Got PRFileDesc* = {}",
          static_cast<unsigned long>(mSessionId), static_cast<void*>(fileDesc));

  MOZ_ASSERT(fileDesc);

  SR_LOGD("[SRParent:{}] Step 17: Converting PRFileDesc to FILE*", static_cast<unsigned long>(mSessionId));
#ifdef XP_WIN
  // Convert our file descriptor to FILE*
  SR_LOGD("[SRParent:{}] Step 17a: Windows platform - converting to native handle", static_cast<unsigned long>(mSessionId));
  void* handle = mozilla::ipc::FileDescriptor::PlatformHandleType(
      PR_FileDesc2NativeHandle(fileDesc));
  SR_LOGD("[SRParent:{}] Step 17b: Got native handle = {}",
          static_cast<unsigned long>(mSessionId), handle);

  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY);
  if (fd == -1) {
    SR_LOGE("[SRParent:{}] Step 17c FAILED: _open_osfhandle failed", static_cast<unsigned long>(mSessionId));
    return;
  }
  SR_LOGD("[SRParent:{}] Step 17c SUCCESS: Got fd = {}", static_cast<unsigned long>(mSessionId), fd);

  FILE* fp = fdopen(fd, "rb");
  if (!fp) {
    SR_LOGE("[SRParent:{}] Step 17d FAILED: fdopen failed", static_cast<unsigned long>(mSessionId));
    return;
  }
#else
  SR_LOGD("[SRParent:{}] Step 17a: Unix platform - converting to native fd", static_cast<unsigned long>(mSessionId));
  PROsfd fd = PR_FileDesc2NativeHandle(fileDesc);
  SR_LOGD("[SRParent:{}] Step 17b: Got native fd = {}", static_cast<unsigned long>(mSessionId), fd);

  FILE* fp = fdopen(fd, "r");
  if (!fp) {
    SR_LOGE("[SRParent:{}] Step 17c FAILED: fdopen failed (errno={})",
            static_cast<unsigned long>(mSessionId), errno);
    return;
  }
#endif
  SR_LOGD("[SRParent:{}] Step 17 SUCCESS: Got FILE* = {}",
          static_cast<unsigned long>(mSessionId), static_cast<void*>(fp));

  SR_LOGD("[SRParent:{}] Step 18: Storing FILE* handle for Whisper initialization", static_cast<unsigned long>(mSessionId));

  // Store the FILE* handle for cleanup later
  mModelFile = fp;
  SR_LOGD("[SRParent:{}] Step 18 SUCCESS: Stored mModelFile = {}",
          static_cast<unsigned long>(mSessionId), static_cast<void*>(mModelFile));

  // We need to initialize Whisper on the background thread
  // Since we don't have direct dispatch to std::thread, we'll use a flag
  SR_LOGD("[SRParent:{}] Step 19: Setting mWhisperInitPending flag to true", static_cast<unsigned long>(mSessionId));
  mWhisperInitPending.store(true);
  SR_LOGD("[SRParent:{}] Step 19 SUCCESS: Flag set", static_cast<unsigned long>(mSessionId));

  SR_LOGD("[SRParent:{}] === END OnModelMetadataReceived (success) ===", static_cast<unsigned long>(mSessionId));
}

SpeechRecognitionParent::~SpeechRecognitionParent() {
  SR_LOGD("[SRParent:{}] Destructor called, mIsActive={}",
       static_cast<unsigned long>(mSessionId), mIsActive ? "true" : "false");
  if (mIsActive) {
    SR_LOGD("[SRParent:{}] Destroying active session, cleaning up", static_cast<unsigned long>(mSessionId));
    mIsActive = false;
  }

  // Stop background thread
  if (mThreadRunning.load()) {
    mThreadRunning.store(false);
    if (mBackgroundThread.joinable()) {
      mBackgroundThread.join();
    }
  }

  // Clean up Whisper context
  CleanupWhisperContext();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvInit(
    const nsCString& aLanguage, InitResolver&& aResolver) {
  SR_LOGD("[SRParent:{}] RecvInit called with language='{}'",
       static_cast<unsigned long>(mSessionId), aLanguage.get());

  mLanguage = aLanguage;
  mIsActive = true;

  // Start background thread for Whisper processing
  if (!mThreadRunning.load()) {
    mThreadRunning.store(true);
    mBackgroundThread = std::thread([self = RefPtr{this}]() {
      self->InitializeWhisperOnBackgroundThread();
      self->ProcessAudioOnBackgroundThread();
    });
    SR_LOGD("[SRParent:{}] Started background processing thread", static_cast<unsigned long>(mSessionId));
  }

  // Since we're using blob-based loading, we'll initialize in the background thread
  // For now, just resolve success
  aResolver(true);
  return IPC_OK();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvProcessAudioData(
    nsTArray<float>&& aAudioData, const uint32_t& aSampleRate) {
  SR_LOGD("[SRParent:{}] RecvProcessAudioData called with {} samples at {} Hz",
       static_cast<unsigned long>(mSessionId), aAudioData.Length(), aSampleRate);

  if (!mIsActive) {
    SR_LOGD("[SRParent:{}] Received audio data but session is not active, ignoring",
         static_cast<unsigned long>(mSessionId));
    return IPC_OK();
  }

  mIPCAudioDumper.Write(aAudioData.Elements(), aAudioData.Length());

  if (!mAudioQueue.Enqueue(aAudioData.Elements(), (int)aAudioData.Length())) {
    SR_LOGD("[SRParent:{}] Audio queue full, dropping sample", static_cast<unsigned long>(mSessionId));
  }

  return IPC_OK();
}

mozilla::ipc::IPCResult SpeechRecognitionParent::RecvStop() {
  SR_LOGD("[SRParent:{}] RecvStop called, mIsActive={}",
       static_cast<unsigned long>(mSessionId), mIsActive ? "true" : "false");

  if (mIsActive) {
    mIsActive = false;

    // Stop background thread
    if (mThreadRunning.load()) {
      mThreadRunning.store(false);
      SR_LOGD("[SRParent:{}] Signaled background thread to stop", static_cast<unsigned long>(mSessionId));
    }

    SR_LOGD("[SRParent:{}] Stopping speech recognition session and cleaning up resources",
         static_cast<unsigned long>(mSessionId));
  } else {
    SR_LOGD("[SRParent:{}] Stop called on inactive session", static_cast<unsigned long>(mSessionId));
  }
  return IPC_OK();
}

void SpeechRecognitionParent::ActorDestroy(ActorDestroyReason aReason) {
  SR_LOGD("[SRParent:{}] ActorDestroy called, reason={}, mIsActive={}",
       static_cast<unsigned long>(mSessionId), static_cast<int>(aReason), mIsActive ? "true" : "false");

  if (mIsActive) {
    SR_LOGD("[SRParent:{}] Actor destroyed while session was active, cleaning up",
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
  SR_LOGD("[SRParent:{}] Initializing Whisper on background thread", static_cast<unsigned long>(mSessionId));

  // Get the runtime linker
  if (!mLib) {
    mLib = mozilla::llama::LlamaRuntimeLinker::Get();
  }

  if (!mLib) {
    SR_LOGE("[SRParent:{}] Failed to get runtime linker", static_cast<unsigned long>(mSessionId));
    return;
  }

  // Dispatch blob retrieval to main thread since it needs to access UtilityProcessChild
  // The actual Whisper initialization will happen in OnModelMetadataReceived when we have the FILE*
  SR_LOGD("[SRParent:{}] Starting blob-based model loading via HWInference", static_cast<unsigned long>(mSessionId));

  RefPtr<SpeechRecognitionParent> self = this;
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "SpeechRecognitionParent::RetrieveModelBlob",
      [self]() {
        self->RetrieveModelBlob();
      }));

  SR_LOGD("[SRParent:{}] Waiting for model blob to be loaded...", static_cast<unsigned long>(mSessionId));
}

void SpeechRecognitionParent::ProcessAudioOnBackgroundThread() {
  SR_LOGD("[SRParent:{}] Starting continuous audio processing loop on background thread",
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

        SR_LOGD("[SRParent:{}] Added {} samples to ring buffer, writePos={}",
             static_cast<unsigned long>(self->mSessionId), dequeued, self->mRingWritePos);
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
      SR_LOGD("[SRParent:{}] === START Whisper Init on Background Thread ===", static_cast<unsigned long>(mSessionId));
      SR_LOGD("[SRParent:{}] Step 20: Found pending init with FILE* = {}",
              static_cast<unsigned long>(mSessionId), static_cast<void*>(mModelFile));

      // Initialize Whisper context with default parameters
      SR_LOGD("[SRParent:{}] Step 21: Getting default Whisper context params", static_cast<unsigned long>(mSessionId));
      struct whisper_context_params cparams = mLib->whisper_context_default_params();
      cparams.use_gpu = true;
      SR_LOGD("[SRParent:{}] Step 21 SUCCESS: Got params (use_gpu={})",
              static_cast<unsigned long>(mSessionId), cparams.use_gpu);

      // Use the FILE* handle to load the model
      SR_LOGD("[SRParent:{}] Step 22: Calling whisper_init_from_file_handle_with_params with FILE* = {}",
              static_cast<unsigned long>(mSessionId), static_cast<void*>(mModelFile));

      // Check if the function pointer is valid
      if (!mLib->whisper_init_from_file_handle_with_params) {
        SR_LOGE("[SRParent:{}] Step 22 FAILED: whisper_init_from_file_handle_with_params is nullptr!",
                static_cast<unsigned long>(mSessionId));
        mWhisperInitPending.store(false);
      } else {
        mWhisperCtx = mLib->whisper_init_from_file_handle_with_params(mModelFile, cparams);

        if (!mWhisperCtx) {
          SR_LOGE("[SRParent:{}] Step 22 FAILED: whisper_init_from_file_handle_with_params returned nullptr",
                  static_cast<unsigned long>(mSessionId));
          fclose(mModelFile);
          mModelFile = nullptr;
        } else {
          SR_LOGD("[SRParent:{}] Step 22 SUCCESS: Got Whisper context = {}",
                  static_cast<unsigned long>(mSessionId), static_cast<void*>(mWhisperCtx));
          SR_LOGD("[SRParent:{}] === END Whisper Init (SUCCESS) ===", static_cast<unsigned long>(mSessionId));
        }

        mWhisperInitPending.store(false);
      }
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

      SR_LOGD("[SRParent:{}] Running recognition on {} samples ({:.2f}s of audio)",
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

        SR_LOGD("[SRParent:{}] Running Whisper inference on {} samples",
             static_cast<unsigned long>(mSessionId), audioForRecognition.size());

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

              SR_LOGD("[SRParent:{}] Whisper result: '{}' (final={})",
                   static_cast<unsigned long>(mSessionId), transcript.get(), isFinal ? "true" : "false");

              // Send result via IPC (dispatch to main thread)
              NS_DispatchToMainThread(NS_NewRunnableFunction(
                  "SpeechRecognitionParent::SendResult",
                  [self = RefPtr{this}, transcript = nsCString(transcript), isFinal]() {
                    if (self->CanSend()) {
                      Unused << self->SendOnRecognitionResult(transcript, isFinal);
                    }
                  }));
            }
          }
        } else {
          SR_LOGD("[SRParent:{}] Whisper inference failed", static_cast<unsigned long>(mSessionId));
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

  SR_LOGD("[SRParent:{}] Continuous audio processing loop terminated", static_cast<unsigned long>(mSessionId));
}

void SpeechRecognitionParent::CleanupWhisperContext() {
  if (mWhisperCtx && mLib) {
    mLib->whisper_free(mWhisperCtx);
    mWhisperCtx = nullptr;
    SR_LOGD("[SRParent:{}] Whisper context cleaned up", static_cast<unsigned long>(mSessionId));
  }

  // Close the model file handle if we have one
  if (mModelFile) {
    fclose(mModelFile);
    mModelFile = nullptr;
    SR_LOGD("[SRParent:{}] Model file handle closed", static_cast<unsigned long>(mSessionId));
  }
}

}  // namespace mozilla::ipc
