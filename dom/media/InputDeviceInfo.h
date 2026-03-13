/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_InputDeviceInfo_h
#define mozilla_dom_InputDeviceInfo_h

#include "mozilla/dom/MediaDeviceInfo.h"
#include "mozilla/dom/MediaTrackCapabilitiesBinding.h"
#include "mozilla/media/MediaUtils.h"

namespace mozilla::dom {

class InputDeviceInfo final : public MediaDeviceInfo {
 public:
  InputDeviceInfo(
      const nsAString& aDeviceId, MediaDeviceKind aKind,
      const nsAString& aLabel, const nsAString& aGroupId,
      RefPtr<media::Refcountable<MediaTrackCapabilities>> aCapabilities);

  NS_DECL_ISUPPORTS_INHERITED

  JSObject* WrapObject(JSContext* cx,
                       JS::Handle<JSObject*> aGivenProto) override;

  void GetCapabilities(MediaTrackCapabilities& aRetVal) const;

 private:
  ~InputDeviceInfo() = default;

  const RefPtr<media::Refcountable<MediaTrackCapabilities>> mCapabilities;
};

}  // namespace mozilla::dom

#endif  // mozilla_dom_InputDeviceInfo_h
