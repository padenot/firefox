/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef AudioSinkInfo_h_
#define AudioSinkInfo_h_

#include "mozilla/dom/AudioContextBinding.h"
#include "nsCycleCollectionParticipant.h"
#include "nsIGlobalObject.h"
#include "nsWrapperCache.h"

namespace mozilla::dom {

// https://webaudio.github.io/web-audio-api/#AudioSinkInfo
class AudioSinkInfo final : public nsISupports, public nsWrapperCache {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(AudioSinkInfo)

  AudioSinkInfo(nsIGlobalObject* aOwner, AudioSinkType aType)
      : mOwner(aOwner), mType(aType) {}

  nsIGlobalObject* GetParentObject() const { return mOwner; }

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  AudioSinkType Type() const { return mType; }

 private:
  ~AudioSinkInfo() = default;

  nsCOMPtr<nsIGlobalObject> mOwner;
  AudioSinkType mType;
};

}  // namespace mozilla::dom

#endif
