/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBNN_ML_H_
#define DOM_WEBNN_ML_H_

#include "mozilla/dom/WebNNBinding.h"
#include "nsIGlobalObject.h"
#include "nsWrapperCache.h"

namespace mozilla::webgpu {
class Device;
}

namespace mozilla::dom {

class Promise;

class ML final : public nsISupports, public nsWrapperCache {
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(ML)

  explicit ML(nsIGlobalObject* aGlobal);

  nsIGlobalObject* GetParentObject() const { return mGlobal; }
  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  already_AddRefed<Promise> CreateContext(const MLContextOptions& aOptions,
                                          ErrorResult& aRv);
  already_AddRefed<Promise> CreateContext(webgpu::Device& aDevice,
                                          ErrorResult& aRv);

 private:
  ~ML() = default;

  nsCOMPtr<nsIGlobalObject> mGlobal;
};

}  // namespace mozilla::dom

#endif  // DOM_WEBNN_ML_H_
