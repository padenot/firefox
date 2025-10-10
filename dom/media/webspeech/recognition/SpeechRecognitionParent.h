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

#include "mozilla/FontPropertyTypes.h"
#include "mozilla/ThreadSafety.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/ipc/PSpeechRecognitionParent.h"
#include "nsCOMPtr.h"
#include "nsISupportsImpl.h"
#include "nsStringFwd.h"
#include "whisper.h"

namespace mozilla::llama {
struct LlamaLibWrapper;
}

namespace mozilla {
struct FCloseDeleter {
  void operator()(FILE* p) {
    if (p) {
      fclose(p);
    }
  }
};
}  // namespace mozilla

namespace mozilla::ipc {

class SpeechRecognitionParent final : public PSpeechRecognitionParent {
 public:
  NS_INLINE_DECL_REFCOUNTING(SpeechRecognitionParent, override)

  SpeechRecognitionParent();

  mozilla::ipc::IPCResult RecvIsModelAvailable(
      const nsTArray<nsCString>& aLanguages,
      IsModelAvailableResolver&& aResolver);
  mozilla::ipc::IPCResult RecvInstallModels(
      const nsTArray<nsCString>& aLanguages, InstallModelsResolver&& aResolver);
  mozilla::ipc::IPCResult RecvInit(const nsCString& aLanguage,
                                   const nsTArray<nsString>& aPhrases,
                                   InitResolver&& aResolver);
  void ActorDestroy(ActorDestroyReason aReason) override;

  struct ModelIdentifier {
    nsCString mModelName;
    nsCString mFileName;
    nsCString mRevision = "main"_ns;
    nsCString ToString() const;
  };

  ModelIdentifier LanguagesToModelIdentifier(
      const nsTArray<nsCString>& aLanguages);

  void ResolveOrRejectInitOnIPCThread(bool aSuccess);

 private:
  ~SpeechRecognitionParent();
  void LoadPreferences();

  void InitializeWhisperContext();
  void RetrieveModel();
  void ProcessAudioOnBackgroundThread();
  void SignalError(const nsCString& aErrorMessage);

  whisper_full_params GetWhisperParams();

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
  mozilla::UniquePtr<FILE, mozilla::FCloseDeleter> mModelFile MOZ_GUARDED_BY(mLock);
  // Whisper instance. Initialized on the background thread, destroyed after
  // thread has been joined on another thread.
  whisper_context* mWhisperCtx;
  InitResolver mInitResolver;
  // Recognition thread started in RecvInit, shut down in destructor
  nsCOMPtr<nsIThread> mRecognitionThread;
  // Flag to signal the recognition thread to stop processing. Set to true when
  // starting, false when we want to stop. Checked periodically by the recognition
  // thread during audio processing.
  std::atomic<bool> mShouldContinueProcessing;
};

}  // namespace mozilla::ipc

#endif  // mozilla_ipc_SpeechRecognitionParent_h
