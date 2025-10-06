/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_SpeechRecognitionParent_h
#define mozilla_ipc_SpeechRecognitionParent_h

#include <atomic>
#include <thread>
#include <vector>

#include "WavDumper.h"
#include "mozilla/FontPropertyTypes.h"
#include "mozilla/SPSCQueue.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/ipc/PSpeechRecognitionParent.h"
#include "nsCOMPtr.h"
#include "nsIFileStreams.h"
#include "nsIInputStream.h"
#include "nsISupportsImpl.h"
#include "nsIThread.h"
#include "nsStringFwd.h"

struct whisper_context;

namespace mozilla::llama {
struct LlamaLibWrapper;
}

namespace mozilla::ipc {

class SpeechRecognitionMetadataCallback;

class SpeechRecognitionParent final : public PSpeechRecognitionParent {
 public:
  NS_INLINE_DECL_REFCOUNTING(SpeechRecognitionParent)

  explicit SpeechRecognitionParent(uint64_t aSessionId);

  mozilla::ipc::IPCResult RecvIsModelAvailable(
      const nsTArray<nsCString>& aLanguages,
      IsModelAvailableResolver&& aResolver);
  mozilla::ipc::IPCResult RecvInstallModels(
      const nsTArray<nsCString>& aLanguages, InstallModelsResolver&& aResolver);
  mozilla::ipc::IPCResult RecvInit(const nsCString& aLanguage,
                                   const nsTArray<nsString>& aPhrases,
                                   InitResolver&& aResolver);
  mozilla::ipc::IPCResult RecvProcessAudioData(nsTArray<float>&& aAudioData);
  mozilla::ipc::IPCResult RecvStop();

  void ActorDestroy(ActorDestroyReason aReason) override;

  uint64_t GetSessionId() const { return mSessionId; }

  // Called when model blob metadata is ready
  void OnModelMetadataReceived();

  struct ModelIdentifier {
    nsCString mModelName;
    nsCString mFileName;
    nsCString mRevision = "main"_ns;
    nsCString ToString() const;
  };

  ModelIdentifier LanguagesToModelIdentifier(
      const nsTArray<nsCString>& aLanguages);

 private:
  ~SpeechRecognitionParent();

  void InitializeWhisperOnBackgroundThread();
  void RetrieveModelBlob();
  void ProcessAudioOnBackgroundThread();
  void CleanupWhisperContext();

  uint64_t mSessionId;
  nsCString mLanguage;
  // Contextual biasing phrases
  nsTArray<nsString> mPhrases;
  // Stream allowing access to model data
  nsCOMPtr<nsIInputStream> mModelStream;
  bool mIsActive;
  RefPtr<SpeechRecognitionMetadataCallback> mMetadataCallback;
  // Model file handle from blob
  FILE* mModelFile = nullptr;
  std::atomic<bool> mWhisperInitPending{false};

  // Whisper-related members
  whisper_context* mWhisperCtx;
  mozilla::llama::LlamaLibWrapper* mLib;
  std::thread mBackgroundThread;
  std::atomic<bool> mThreadRunning;

  // Audio processing members
  mozilla::SPSCQueue<float> mAudioQueue;
  std::vector<float> mAudioRing;
  size_t mRingWritePos;
  size_t mRingSize;

  // Whisper parameters
  // How often recognition is ran
  int32_t mRecognitionIntervalMs;
  // Duration of a segment sent to whisper each time
  int32_t mAudioLengthMs;
  int32_t mNumThreads;

  // Dumps audio sent to Whisper. This will contain segments of about 10s of
  // audio, representing the audio sent to whisper.
  WavDumper mWhisperAudioDumper;
};

}  // namespace mozilla::ipc

#endif  // mozilla_ipc_SpeechRecognitionParent_h
