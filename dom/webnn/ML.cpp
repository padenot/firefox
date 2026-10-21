/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/ML.h"

#include "mozilla/dom/MLContext.h"
#include "mozilla/dom/Promise.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(ML, mGlobal)
NS_IMPL_CYCLE_COLLECTING_ADDREF(ML)
NS_IMPL_CYCLE_COLLECTING_RELEASE(ML)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(ML)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

ML::ML(nsIGlobalObject* aGlobal) : mGlobal(aGlobal) {}

JSObject* ML::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto) {
  return ML_Binding::Wrap(aCx, this, aGivenProto);
}

already_AddRefed<Promise> ML::CreateContext(const MLContextOptions& aOptions,
                                            ErrorResult& aRv) {
  RefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  MLContext::Create(mGlobal, aOptions, promise);
  return promise.forget();
}

already_AddRefed<Promise> ML::CreateContext(webgpu::Device& aDevice,
                                            ErrorResult& aRv) {
  RefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }
  promise->MaybeRejectWithNotSupportedError("WebGPU interop is not supported");
  return promise.forget();
}

}  // namespace mozilla::dom
