/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionPermissionRequest.h"

#include "SpeechRecognition.h"
#include "SpeechRecognitionBackend.h"
#include "SpeechRecognitionModels.h"
#include "mozilla/Preferences.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "nsContentPermissionHelper.h"
#include "nsGlobalWindowInner.h"

namespace mozilla::dom {

// A native Promise handler that:
// - forwards the resolved boolean to the outer mPromise
// - removes the languages from SpeechRecognition::sDownloadingLanguages
class SpeechRecognitionInstallHandler final : public PromiseNativeHandler {
 public:
  NS_DECL_ISUPPORTS

  SpeechRecognitionInstallHandler(Promise* aOuterPromise,
                                  nsTArray<nsCString>&& aLanguages)
      : mOuterPromise(aOuterPromise), mLanguages(std::move(aLanguages)) {}

  void ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                        ErrorResult& aRv) override {
    bool success = aValue.isBoolean() && aValue.toBoolean();
    mOuterPromise->MaybeResolve(success);
    Cleanup();
  }

  void RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                        ErrorResult& aRv) override {
    mOuterPromise->MaybeResolve(false);
    Cleanup();
  }

 private:
  ~SpeechRecognitionInstallHandler() = default;

  void Cleanup() {
    AssertIsOnMainThread();
    for (const nsCString& lang : mLanguages) {
      SpeechRecognition::RemoveDownloadingLanguage(lang);
    }
  }

  RefPtr<Promise> mOuterPromise;
  nsTArray<nsCString> mLanguages;
};

NS_IMPL_ISUPPORTS0(SpeechRecognitionInstallHandler)

NS_IMPL_CYCLE_COLLECTION_INHERITED(SpeechRecognitionPermissionRequest,
                                   ContentPermissionRequestBase, mPromise)

NS_IMPL_QUERY_INTERFACE_CYCLE_COLLECTION_INHERITED(
    SpeechRecognitionPermissionRequest, ContentPermissionRequestBase,
    nsIRunnable)

NS_IMPL_ADDREF_INHERITED(SpeechRecognitionPermissionRequest,
                         ContentPermissionRequestBase)
NS_IMPL_RELEASE_INHERITED(SpeechRecognitionPermissionRequest,
                          ContentPermissionRequestBase)

// Returns the size in MB of the model that will be downloaded for the given
// language set, mirroring the selection logic in LanguagesToModelIdentifier.
static uint32_t ModelSizeMB(const nsTArray<nsString>& aLanguages) {
  // Derive the locale prefix the same way LanguagesToModelIdentifier does.
  nsCString prefix;
  if (!aLanguages.IsEmpty()) {
    prefix = NS_ConvertUTF16toUTF8(aLanguages[0]);
    int32_t dash = prefix.FindChar('-');
    if (dash != kNotFound) {
      prefix.Truncate(dash);
    }
  }

  nsAutoCString prefKey("media.webspeech.recognition.model.");
  prefKey.Append(prefix.IsEmpty() ? "multilingual"_ns : prefix);
  nsAutoCString prefModelId;
  Preferences::GetCString(prefKey.get(), prefModelId);

  if (!prefModelId.IsEmpty()) {
    for (const auto& m : kSpeechRecognitionModels) {
      if (m.id && prefModelId.Equals(m.id)) {
        return m.size_mb;
      }
    }
  }

  // No pref: use the default model for this locale prefix.
  uint32_t fallbackSize = 0;
  for (const auto& m : kSpeechRecognitionModels) {
    if (!m.id) {
      break;
    }
    if (!m.locales[0]) {
      if (m.is_default && !fallbackSize) {
        fallbackSize = m.size_mb;
      }
      continue;
    }
    for (const char* const* l = m.locales; *l; ++l) {
      if (StringBeginsWith(prefix, nsDependentCString(*l)) ||
          prefix.IsEmpty()) {
        if (m.is_default) {
          return m.size_mb;
        }
      }
    }
  }
  return fallbackSize;
}

SpeechRecognitionPermissionRequest::SpeechRecognitionPermissionRequest(
    nsPIDOMWindowInner* aWindow, Promise* aPromise,
    const nsTArray<nsString>& aLanguages)
    : ContentPermissionRequestBase(aWindow->GetDoc()->NodePrincipal(), aWindow,
                                   ""_ns,
                                   "speech-recognition-model-download"_ns),
      mPromise(aPromise),
      mLanguages(aLanguages.Clone()) {}

NS_IMETHODIMP
SpeechRecognitionPermissionRequest::GetTypes(nsIArray** aTypes) {
  nsTArray<nsString> options;
  options.AppendElement(
      NS_ConvertUTF8toUTF16(nsPrintfCString("%u", ModelSizeMB(mLanguages))));
  return nsContentPermissionUtils::CreatePermissionArray(mType, options,
                                                         aTypes);
}

NS_IMETHODIMP
SpeechRecognitionPermissionRequest::Cancel() {
  mPromise->MaybeResolve(false);
  return NS_OK;
}

NS_IMETHODIMP
SpeechRecognitionPermissionRequest::Allow(JS::Handle<JS::Value> aChoices) {
  AssertIsOnMainThread();
  MOZ_ASSERT(aChoices.isUndefined());

  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(mWindow.get());
  if (!global) {
    mPromise->MaybeResolve(false);
    return NS_OK;
  }

  nsTArray<nsCString> languagesUtf8;
  for (const nsString& lang : mLanguages) {
    languagesUtf8.AppendElement(NS_ConvertUTF16toUTF8(lang));
    SpeechRecognition::AddDownloadingLanguage(NS_ConvertUTF16toUTF8(lang));
  }

  RefPtr<Promise> installPromise =
      SpeechRecognitionBackend::Install(global, mLanguages);
  if (!installPromise) {
    for (const nsCString& lang : languagesUtf8) {
      SpeechRecognition::RemoveDownloadingLanguage(lang);
    }
    mPromise->MaybeResolve(false);
    return NS_OK;
  }

  auto handler = MakeRefPtr<SpeechRecognitionInstallHandler>(
      mPromise, std::move(languagesUtf8));
  installPromise->AppendNativeHandler(handler);

  return NS_OK;
}

NS_IMETHODIMP
SpeechRecognitionPermissionRequest::Run() {
  if (Preferences::GetBool(
          "media.webspeech.recognition.model-download.prompt.testing", false)) {
    if (Preferences::GetBool("media.navigator.permission.disabled", false)) {
      Allow(JS::UndefinedHandleValue);
    } else {
      Cancel();
    }
    return NS_OK;
  }
  if (NS_FAILED(nsContentPermissionUtils::AskPermission(this, mWindow))) {
    Cancel();
  }
  return NS_OK;
}

}  // namespace mozilla::dom
