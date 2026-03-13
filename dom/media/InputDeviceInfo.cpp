/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/InputDeviceInfo.h"

#include "mozilla/dom/InputDeviceInfoBinding.h"

namespace mozilla::dom {

InputDeviceInfo::InputDeviceInfo(
    const nsAString& aDeviceId, MediaDeviceKind aKind, const nsAString& aLabel,
    const nsAString& aGroupId,
    RefPtr<media::Refcountable<MediaTrackCapabilities>> aCapabilities)
    : MediaDeviceInfo(aDeviceId, aKind, aLabel, aGroupId),
      mCapabilities(std::move(aCapabilities)) {}

NS_IMPL_ISUPPORTS_CYCLE_COLLECTION_INHERITED_0(InputDeviceInfo, MediaDeviceInfo)

JSObject* InputDeviceInfo::WrapObject(JSContext* aCx,
                                      JS::Handle<JSObject*> aGivenProto) {
  return InputDeviceInfo_Binding::Wrap(aCx, this, aGivenProto);
}

void InputDeviceInfo::GetCapabilities(MediaTrackCapabilities& aRetVal) const {
  MOZ_ASSERT(NS_IsMainThread());
  aRetVal = *mCapabilities;
}

}  // namespace mozilla::dom
