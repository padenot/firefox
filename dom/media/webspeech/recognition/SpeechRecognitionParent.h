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
#include "mozilla/dom/Promise.h"
#include "mozilla/ipc/PSpeechRecognitionParent.h"
#include "nsCOMPtr.h"
#include "nsISupportsImpl.h"
#include "nsStringFwd.h"

namespace mozilla::ipc {

class SpeechRecognitionMetadataCallback;

class SpeechRecognitionParent final : public PSpeechRecognitionParent {
 public:
  NS_INLINE_DECL_REFCOUNTING(SpeechRecognitionParent)

  SpeechRecognitionParent();

  mozilla::ipc::IPCResult RecvIsModelAvailable(
      const nsTArray<nsCString>& aLanguages,
      IsModelAvailableResolver&& aResolver);
  mozilla::ipc::IPCResult RecvInstallModels(
      const nsTArray<nsCString>& aLanguages, InstallModelsResolver&& aResolver);
  void ActorDestroy(ActorDestroyReason aReason) override;


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

  // Static tracking of the single active recognition session
  static StaticMutex sSessionMutex;
  static StaticRefPtr<SpeechRecognitionParent> sActiveSession MOZ_GUARDED_BY(sSessionMutex);

  Mutex mLock;
  // Recognition language
  // Set during RecvInit, then constant
  nsCString mLanguage ;//MOZ_GUARDED_BY(mLock);
  // Contextual biasing phrases
  // Set during RecvInit, then constant
  nsTArray<nsString> mPhrases; //MOZ_GUARDED_BY(mLock);
};

}  // namespace mozilla::ipc

#endif  // mozilla_ipc_SpeechRecognitionParent_h
