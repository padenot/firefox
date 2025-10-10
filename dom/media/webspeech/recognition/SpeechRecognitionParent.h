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
#include "mozilla/MozPromise.h"
#include "mozilla/PSpeechRecognitionParent.h"
#include "mozilla/SPSCQueue.h"
#include "mozilla/ThreadSafety.h"
#include "mozilla/dom/Promise.h"
#include "nsCOMPtr.h"
#include "nsISupportsImpl.h"
#include "nsStringFwd.h"

namespace mozilla::hwinference {
class HWInferenceChild;
}

namespace mozilla {

class SpeechRecognitionParent final : public PSpeechRecognitionParent {
 public:
  NS_INLINE_DECL_REFCOUNTING(SpeechRecognitionParent, override)

  SpeechRecognitionParent();

  ipc::IPCResult RecvIsModelAvailable(const nsTArray<nsCString>& aLanguages,
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

  // Shared by RecvIsModelAvailable and RecvInstallModels, which otherwise
  // only differ in the HWInferenceChild call they make. Resolves
  // aResolver(false) if the utility process/HWInferenceChild isn't
  // available; otherwise calls aSendFunc(hwInferenceChild) and resolves
  // aResolver with the result (false on IPC rejection).
  mozilla::ipc::IPCResult RunHWInferenceBoolQuery(
      const char* aFuncName,
      std::function<RefPtr<MozPromise<bool, ipc::ResponseRejectReason, true>>(
          hwinference::HWInferenceChild*)>
          aSendFunc,
      std::function<void(const bool&)> aResolver);
};

}  // namespace mozilla

#endif  // DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITIONPARENT_H_
