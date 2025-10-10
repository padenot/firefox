/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionPhrase.h"

#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/SpeechRecognitionPhraseBinding.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(SpeechRecognitionPhrase, mGlobal)

SpeechRecognitionPhrase::SpeechRecognitionPhrase(nsIGlobalObject* aGlobal)
    : mGlobal(aGlobal), mBoost(1.0f) {}

already_AddRefed<SpeechRecognitionPhrase> SpeechRecognitionPhrase::Constructor(
    const GlobalObject& aGlobal, const nsAString& aPhrase, float aBoost,
    ErrorResult& aRv) {
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());
  if (!global) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  if (aBoost < 0.0f || aBoost > 10.0f) {
    aRv.ThrowSyntaxError("Boost value must be in range [0.0, 10.0]");
    return nullptr;
  }

  RefPtr<SpeechRecognitionPhrase> phrase = new SpeechRecognitionPhrase(global);
  phrase->Initialize(aPhrase, aBoost);

  return phrase.forget();
}

void SpeechRecognitionPhrase::Initialize(const nsAString& aPhrase,
                                         float aBoost) {
  mPhrase = aPhrase;
  mBoost = aBoost;
}

JSObject* SpeechRecognitionPhrase::WrapObject(
    JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return SpeechRecognitionPhrase_Binding::Wrap(aCx, this, aGivenProto);
}

}  // namespace mozilla::dom
