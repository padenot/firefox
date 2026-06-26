/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_SpeechRecognitionPermissionRequest_h
#define mozilla_dom_SpeechRecognitionPermissionRequest_h

#include "mozilla/dom/Promise.h"
#include "nsContentPermissionHelper.h"
#include "nsString.h"
#include "nsTArray.h"

namespace mozilla::dom {

/**
 * Presents a permission doorhanger asking the user to consent to downloading
 * the on-device speech recognition model before SpeechRecognition.install()
 * initiates a potentially large download. On Allow(), the install proceeds;
 * on Cancel() the promise is resolved false.
 */
class SpeechRecognitionPermissionRequest final
    : public ContentPermissionRequestBase,
      public nsIRunnable {
 public:
  // aSizeMB is the model download size shown in the prompt, computed in the
  // utility process so the content process does not need the model table.
  SpeechRecognitionPermissionRequest(nsPIDOMWindowInner* aWindow,
                                     Promise* aPromise,
                                     const nsTArray<nsString>& aLanguages,
                                     uint32_t aSizeMB);

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIRUNNABLE
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(SpeechRecognitionPermissionRequest,
                                           ContentPermissionRequestBase)

  NS_IMETHOD Cancel() override;
  NS_IMETHOD Allow(JS::Handle<JS::Value> aChoices) override;
  NS_IMETHOD GetTypes(nsIArray** aTypes) override;

 private:
  ~SpeechRecognitionPermissionRequest() = default;

  RefPtr<Promise> mPromise;
  nsTArray<nsString> mLanguages;
  uint32_t mSizeMB;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_SpeechRecognitionPermissionRequest_h
