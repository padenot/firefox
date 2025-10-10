/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8  et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITIONPARENT_H_
#define DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITIONPARENT_H_

#include <atomic>
#include <functional>

#include "WavDumper.h"
#include "mozilla/FileUtils.h"
#include "mozilla/MozPromise.h"
#include "mozilla/PSpeechRecognitionParent.h"
#include "mozilla/SPSCQueue.h"
#include "mozilla/ThreadSafety.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/hwinference/PHWInferenceChild.h"
#include "nsCOMPtr.h"
#include "nsISupportsImpl.h"
#include "nsIThread.h"
#include "nsStringFwd.h"

namespace mozilla::llama {
struct LlamaLibWrapper;
}

namespace mozilla::hwinference {
class HWInferenceChild;
}

// Opaque handles from mudler/parakeet.cpp's streaming C-API (parakeet_capi.h).
struct parakeet_ctx;
struct parakeet_stream;

namespace mozilla {

class SpeechRecognitionParent final : public PSpeechRecognitionParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(SpeechRecognitionParent, override)

  SpeechRecognitionParent();

  ipc::IPCResult RecvIsModelAvailable(const nsTArray<nsCString>& aLanguages,
                                      IsModelAvailableResolver&& aResolver);
  mozilla::ipc::IPCResult RecvInstallModels(
      const nsTArray<nsCString>& aLanguages, InstallModelsResolver&& aResolver);
  mozilla::ipc::IPCResult RecvInit(const nsCString& aEngineId,
                                   const nsCString& aLanguage,
                                   const nsTArray<nsString>& aPhrases,
                                   InitResolver&& aResolver);
  mozilla::ipc::IPCResult RecvProcessAudioData(nsTArray<float>&& aAudioData);
  mozilla::ipc::IPCResult RecvStop();

  void ActorDestroy(ActorDestroyReason aReason) override;

  struct ModelIdentifier {
    nsCString mModelName;
    nsCString mFileName;
    nsCString mRevision = "main"_ns;
    nsCString ToString() const;
  };

  ModelIdentifier LanguagesToModelIdentifier(
      const nsTArray<nsCString>& aLanguages);

  void ResolveOrRejectInitOnIPCThread(InitResolver&& aResolver, bool aSuccess)
      MOZ_EXCLUDES(mLock);

 private:
  ~SpeechRecognitionParent();
  void LoadPreferences();

  // Shared by RecvIsModelAvailable and RecvInstallModels, which otherwise
  // only differ in the HWInferenceChild call they make. Resolves
  // aResolver(false) if the utility process/HWInferenceChild isn't
  // available; otherwise calls aSendFunc(hwInferenceChild), tracks the
  // resulting promise in aRequestHolder, and resolves aResolver with the
  // result (false on IPC rejection).
  using BoolPromise = hwinference::PHWInferenceChild::IsModelAvailablePromise;
  mozilla::ipc::IPCResult RunHWInferenceBoolQuery(
      const char* aFuncName,
      std::function<RefPtr<BoolPromise>(hwinference::HWInferenceChild*)>
          aSendFunc,
      std::function<void(const bool&)> aResolver,
      MozPromiseRequestHolder<BoolPromise>& aRequestHolder);

  void InitializeParakeetContext(InitResolver&& aResolver);
  void RetrieveModel(InitResolver&& aResolver);
  // Cache-aware streaming path (mudler/parakeet.cpp streaming C-API).
  void ProcessAudioStreaming();
  void SignalError(const nsCString& aErrorMessage);

  // Static tracking of the single active recognition session
  static StaticMutex sSessionMutex;
  static StaticRefPtr<SpeechRecognitionParent> sActiveSession
      MOZ_GUARDED_BY(sSessionMutex);

  Mutex mLock;
  // Recognition language
  // Set during RecvInit, then constant
  nsCString mLanguage MOZ_GUARDED_BY(mLock);
  // Contextual biasing phrases
  // Set during RecvInit, then constant
  nsTArray<nsString> mPhrases MOZ_GUARDED_BY(mLock);
  // Model file handle - automatically closed on destruction
  // ScopedCloseFile is UniquePtr<FILE, FCloseDeleter>
  mozilla::UniquePtr<FILE, mozilla::FCloseDeleter> mModelFile
      MOZ_GUARDED_BY(mLock);
  // Streaming backend handles (mudler/parakeet.cpp). Created on the
  // recognition thread, freed in ActorDestroy.
  parakeet_ctx* mCapiCtx = nullptr;
  parakeet_stream* mCapiStream = nullptr;

  // Lock-free queue to convey audio from the IPC thread to the processing
  // thread. Producer is the IPC thread, consumer is the processing thread.
  mozilla::SPSCQueue<float> mAudioQueue;

  // Started in RecvInit, then stopped and join on actor destroyed, recognitions
  // stopped, etc.
  nsCOMPtr<nsIThread> mRecognitionThread;

  // Tunable parameters for recognition, bound to prefs.
  struct RecognitionParams {
    // Latency & Timing
    int32_t mRecognitionIntervalMs = 1000;  // How often to run recognition
    int32_t mKeepAudioMs = 1000;            // Audio overlap between segments

    // VAD Parameters -- not wired up yet
    bool mUseVAD = false;               // Enable VAD pre-filtering
    float mVADThreshold = 0.6f;         // VAD activation threshold
    int32_t mVADMinSpeechMs = 250;      // Min speech duration
    int32_t mVADMinSilenceMs = 2000;    // Min silence before cutting
    float mVADEnergyThreshold = 0.01f;  // Energy-based VAD
    int32_t mVADSpeechPadMs = 300;      // Padding around speech

    // Context & Memory
    int32_t mMaxTextContext = 16384;  // Max text context chars
  };

  RecognitionParams mParams;

  // Dumps audio sent to the recognizer. This will contain segments of about
  // 10s of audio, representing the audio sent for inference.
  // MOZ_DISABLE_UTILITY_SANDBOX=1 MOZ_DUMP_AUDIO=1 to activate
  WavDumper mRecognitionAudioDumper;

  // Flag to signal the recognition thread to stop processing. Set to true when
  // starting, false when we want to stop. Checked periodically by the
  // recognition thread during audio processing.
  std::atomic<bool> mShouldContinueProcessing;

  // Set once ActorDestroy() has run. InitializeParakeetContext() can still be
  // mid-flight on the recognition thread when that happens (e.g. delayed
  // behind a model fetch); this tells it to discard its work instead of
  // resurrecting mShouldContinueProcessing and starting a streaming loop
  // nobody will ever stop.
  std::atomic<bool> mActorDestroyed{false};

  // Position in the audio stream that has been processed in samples
  // This provides a rather crude timing estimate, but will be improved.
  size_t mProcessedAudioPos;

  // Outstanding requests to the utility process, disconnected in
  // ActorDestroy() so their callbacks never run (and resolve a dead IPDL
  // resolver) after the actor is torn down.
  MozPromiseRequestHolder<
      hwinference::PHWInferenceChild::IsModelAvailablePromise>
      mIsModelAvailableRequest;
  MozPromiseRequestHolder<hwinference::PHWInferenceChild::InstallModelPromise>
      mInstallModelRequest;
  MozPromiseRequestHolder<hwinference::PHWInferenceChild::GetModelFilePromise>
      mGetModelFileRequest;
};

}  // namespace mozilla

#endif  // DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITIONPARENT_H_
