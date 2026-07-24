/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BrowserInferenceExampleParent.h"

#include "mozilla/Logging.h"
#include "mozilla/hwinference/HWInferenceChild.h"
#include "mozilla/ipc/UtilityProcessChild.h"
#include "mozilla/llama/LlamaRuntimeLinker.h"
#include "nsThreadUtils.h"

namespace mozilla::hwinference {

extern LazyLogModule gHWInferenceLog;
#define LOGD(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Debug, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) \
  MOZ_LOG_FMT(gHWInferenceLog, LogLevel::Error, fmt, ##__VA_ARGS__)

static BrowserInferenceExampleResult SmokeErrorResult(const char* aMessage) {
  return BrowserInferenceExampleResult(false, 0.0f, nsCString(aMessage));
}

static BrowserInferenceExampleResult RunGgmlScalarSmokeTest(float aInput) {
  mozilla::llama::LlamaLibWrapper* lib =
      mozilla::llama::LlamaRuntimeLinker::Get();
  if (!lib) {
    return SmokeErrorResult("mozinference link failed");
  }

  ggml_init_params params = {
      .mem_size = 256 * 1024,
      .mem_buffer = nullptr,
      .no_alloc = false,
  };

  ggml_context* ctx = lib->ggml_init(params);
  if (!ctx) {
    return SmokeErrorResult("ggml_init failed");
  }

  ggml_tensor* x = lib->ggml_new_f32(ctx, aInput);
  ggml_tensor* one = lib->ggml_new_f32(ctx, 1.0f);
  ggml_tensor* y = lib->ggml_add(ctx, lib->ggml_mul(ctx, x, x), one);
  ggml_cgraph* graph = lib->ggml_new_graph(ctx);

  if (!x || !one || !y || !graph) {
    lib->ggml_free(ctx);
    return SmokeErrorResult("ggml graph allocation failed");
  }

  lib->ggml_build_forward_expand(graph, y);
  ggml_status status = lib->ggml_graph_compute_with_ctx(ctx, graph, 1);
  if (status != GGML_STATUS_SUCCESS) {
    lib->ggml_free(ctx);
    return SmokeErrorResult("ggml graph compute failed");
  }

  float value = lib->ggml_get_f32_1d(y, 0);
  lib->ggml_free(ctx);
  return BrowserInferenceExampleResult(true, value, ""_ns);
}

mozilla::ipc::IPCResult BrowserInferenceExampleParent::RecvRunScalar(
    float aInput, RunScalarResolver&& aResolver) {
  LOGD("BrowserInferenceExampleParent::RecvRunScalar input={}", aInput);

  RefPtr<HWInferenceChild> hwInference;
  if (RefPtr<ipc::UtilityProcessChild> utilityChild =
          ipc::UtilityProcessChild::GetSingleton()) {
    hwInference = utilityChild->GetHWInferenceChild();
  }
  if (!hwInference) {
    aResolver(SmokeErrorResult("no HWInferenceChild in utility process"));
    return IPC_OK();
  }

  // Before running compute, exercise the generic model-provisioning path so the
  // demo goes through the consumer's registered resolver and download gate:
  // InstallModel runs this task's nsIMLModelResolver (opaque id -> concrete
  // ModelHub artifact) and its nsIMLModelDownloadGate (authorization) in the
  // main process; IsModelInstalled then re-runs the resolver and confirms the
  // install took effect. Both round-trips happen entirely across processes.
  nsCString task("browser-inference-example"_ns);
  nsCString id("demo-model"_ns);

  hwInference
      ->SendInstallModel(task, id, /* aInnerWindowId */ 0,
                         dom::ContentParentId(), /* aProgressToken */ nsString())
      ->Then(
          GetMainThreadSerialEventTarget(), __func__,
          [self = RefPtr{this}, hwInference, task, id, aInput,
           aResolver = std::move(aResolver)](
              HWInferenceChild::InstallModelPromise::ResolveOrRejectValue&&
                  aInstall) mutable {
            if (!aInstall.IsResolve() || !aInstall.ResolveValue()) {
              aResolver(SmokeErrorResult(
                  "InstallModel (resolver/gate) did not authorize"));
              return;
            }
            hwInference->SendIsModelInstalled(task, id)->Then(
                GetMainThreadSerialEventTarget(), __func__,
                [aInput, aResolver = std::move(aResolver)](
                    HWInferenceChild::IsModelInstalledPromise::
                        ResolveOrRejectValue&& aInstalled) mutable {
                  if (!aInstalled.IsResolve() || !aInstalled.ResolveValue()) {
                    aResolver(SmokeErrorResult(
                        "model not installed after InstallModel"));
                    return;
                  }
                  aResolver(RunGgmlScalarSmokeTest(aInput));
                });
          });

  return IPC_OK();
}

void BrowserInferenceExampleParent::ActorDestroy(ActorDestroyReason aReason) {
  LOGD("BrowserInferenceExampleParent::ActorDestroy reason={}",
       static_cast<int>(aReason));
}

}  // namespace mozilla::hwinference

#undef LOGD
#undef LOGE
