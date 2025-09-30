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
#include "mozilla/SPSCQueue.h"
#include "mozilla/ipc/PSpeechRecognitionParent.h"
#include "mozilla/dom/Promise.h"
#include "nsISupportsImpl.h"
#include "nsIThread.h"
#include "nsIInputStream.h"
#include "nsCOMPtr.h"
#include "nsIFileStreams.h"

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
                                   InitResolver&& aResolver);
  mozilla::ipc::IPCResult RecvProcessAudioData(nsTArray<float>&& aAudioData,
                                               const uint32_t& aSampleRate);
  mozilla::ipc::IPCResult RecvStop();

  void ActorDestroy(ActorDestroyReason aReason) override;

  uint64_t GetSessionId() const { return mSessionId; }

  // Called when model blob metadata is ready
  void OnModelMetadataReceived();

 private:
  ~SpeechRecognitionParent();

  void InitializeWhisperOnBackgroundThread();
  void ProcessAudioOnBackgroundThread();
  void CleanupWhisperContext();

  // Retrieve model blob and convert to file descriptor for Whisper
  void RetrieveModelBlob();

  uint64_t mSessionId;
  nsCString mLanguage;
  nsCString mModelPath;  // Path to the model file (will be replaced with fd)
  nsCOMPtr<nsIInputStream> mModelStream;  // Stream for model blob
  // int mModelFd;  // File descriptor for model (when whisper supports it)
  bool mIsActive;

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
  int32_t mRecognitionIntervalMs;  // How often to run recognition (1000ms = 1
                                   // second)
  int32_t mAudioLengthMs;  // Length of audio to analyze (e.g., 10 seconds)
  int32_t mNumThreads;

  // Audio dumpers for debugging
  WavDumper mIPCAudioDumper;      // Dumps audio received via IPC
  WavDumper mWhisperAudioDumper;  // Dumps audio sent to Whisper

  RefPtr<SpeechRecognitionMetadataCallback> mMetadataCallback;

  // Model file handle from blob
  FILE* mModelFile = nullptr;
  std::atomic<bool> mWhisperInitPending{false};
};

} // namespace mozilla::ipc


#endif  // mozilla_ipc_SpeechRecognitionParent_h
