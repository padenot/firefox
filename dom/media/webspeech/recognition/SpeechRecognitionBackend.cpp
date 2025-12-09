/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2  et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <type_traits>

#include "SpeechRecognition.h"
#include "SpeechTrackListener.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/Assertions.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/dom/AudioStreamTrack.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/SpeechRecognitionBinding.h"
#include "mozilla/hwinference/HWInferenceManagerChild.h"
#include "mozilla/hwinference/SpeechRecognitionChild.h"
#include "mozilla/ipc/MessageChannel.h"
#include "mozilla/ipc/ProtocolUtils.h"

namespace mozilla::dom {

using namespace mozilla::ipc;

StaticAutoPtr<mozilla::EventTargetCapability<nsISerialEventTarget>>
    SpeechRecognitionBackend::sIPCCapability;
int32_t SpeechRecognitionBackend::sIPCActorUsers = 0;
StaticRefPtr<nsITimer> SpeechRecognitionBackend::sIdleCloseTimer;

void IPCActorUserCounter::Increment() { mCount++; }

void IPCActorUserCounter::Decrement() {
  MOZ_ASSERT(mCount > 0);
  mCount--;
  if (!mCount) {
    SpeechRecognitionBackend::CloseIPCActorIfUnused();
  }
}

bool IPCActorUserCounter::IsZero() const { return !mCount; }

/* static */
void SpeechRecognitionBackend::CloseHWInferenceChildIfAny() {
  AssertOnIPCThread();
  RefPtr<mozilla::hwinference::HWInferenceManagerChild> child =
      mozilla::hwinference::HWInferenceManagerChild::GetSingleton();
  if (child) {
    child->Close();
  }
}

static LazyLogModule gSpeechRecognitionBackendLog("SpeechRecognitionBackend");

#define LOG(fmt, ...)                                                      \
  MOZ_LOG_FMT(gSpeechRecognitionBackendLog, mozilla::LogLevel::Debug, fmt, \
              ##__VA_ARGS__)
#define LOGV(fmt, ...)                                                       \
  MOZ_LOG_FMT(gSpeechRecognitionBackendLog, mozilla::LogLevel::Verbose, fmt, \
              ##__VA_ARGS__)
#define LOGE(fmt, ...)                                                     \
  MOZ_LOG_FMT(gSpeechRecognitionBackendLog, mozilla::LogLevel::Error, fmt, \
              ##__VA_ARGS__)

// How long the shared IPC thread stays idle before its backing OS thread is
// released. The serial event target itself lives for the process lifetime.
static constexpr uint32_t IPC_THREAD_IDLE_TIMEOUT_MS = 5000;

SpeechRecognitionBackend::IPCActorUserGuard::~IPCActorUserGuard() {
  if (NS_IsMainThread()) {
    SpeechRecognitionBackend::ReleaseIPCActorUser();
  } else {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "IPCActorUserGuard::Release",
        [] { SpeechRecognitionBackend::ReleaseIPCActorUser(); }));
  }
}

SpeechRecognitionBackend::SpeechRecognitionBackend(
    SpeechRecognition* aParent, uint32_t aGraphRate, const nsString& aLanguage,
    const nsTArray<nsString>& aPhrases)
    : mParent(aParent),
      mLanguage(NS_ConvertUTF16toUTF8(aLanguage)),
      mPhrases(aPhrases.Clone()) {}

SpeechRecognitionBackend::~SpeechRecognitionBackend() { Abort(); }

nsresult SpeechRecognitionBackend::Start() { return NS_OK; }

void SpeechRecognitionBackend::Stop() {}

void SpeechRecognitionBackend::Abort() {
  AssertIsOnMainThread();
  LOG("SpeechRecognitionBackend::Abort");
  Stop();
}

void SpeechRecognitionBackend::AttachToTrack(AudioStreamTrack* aTrack) {
  AssertIsOnMainThread();
  MOZ_ASSERT(aTrack);
}

void SpeechRecognitionBackend::DetachFromTrack() { AssertIsOnMainThread(); }

void SpeechRecognitionBackend::DataCallback(TrackTime aTime,
                                            const AudioChunk& aChunk) {}

void SpeechRecognitionBackend::NotifyTrackEnded() {}

/* static */
nsCOMPtr<nsISerialEventTarget>
SpeechRecognitionBackend::GetOrCreateIPCThread() {
  AssertIsOnMainThread();

  if (!sIPCCapability) {
    // IPC actors are bound to the event target they were opened on, so the
    // target has to outlive them: a LazyIdleThread keeps a single one for the
    // process lifetime, and only releases its backing OS thread when idle.
    RefPtr<LazyIdleThread> thread =
        new LazyIdleThread(IPC_THREAD_IDLE_TIMEOUT_MS, "SpeechIPC");
    sIPCCapability = new EventTargetCapability<nsISerialEventTarget>(thread);
    LOG("Created shared IPC thread for speech recognition");
    ClearOnShutdown(&sIPCCapability);
  }

  return nsCOMPtr<nsISerialEventTarget>(sIPCCapability->GetEventTarget());
}

void SpeechRecognitionBackend::CloseIPCActorIfUnused() {
  AssertIsOnMainThread();
  // Close the HWInference connection once no session needs it, so the utility
  // process isn't kept alive by an idle connection. The serial event target is
  // kept (its backing OS thread is released on idle by the LazyIdleThread);
  // the next EnsureIPC() reopens the connection on that same target.
  if (!sIPCActorUsers && sIPCCapability) {
    nsCOMPtr<nsIRunnable> close = NS_NewRunnableFunction(
        "SpeechRecognitionBackend::CloseHWInferenceChildIfAny",
        [] { CloseHWInferenceChildIfAny(); });
    sIPCCapability->Dispatch(close.forget());
  }
}

/* static */
void SpeechRecognitionBackend::AssertOnIPCThread() {
  sIPCCapability->AssertOnCurrentThread();
}

/* static */
template <typename SendFunc, typename OnResult, typename OnFailure>
void SpeechRecognitionBackend::RunWithTransientSession(
    nsTArray<nsCString>&& aLanguages, SendFunc aSendFunc, OnResult aOnResult,
    OnFailure aOnFailure) {
  AssertIsOnMainThread();

  using SendPromise = std::remove_pointer_t<
      decltype(aSendFunc(
                   static_cast<hwinference::SpeechRecognitionChild*>(nullptr),
                   aLanguages)
                   .get())>;
  using ResolveValueType = typename SendPromise::ResolveValueType;
  // Carries the outcome back to the main thread: aOnResult and aOnFailure
  // hold DOM promises, which must not be touched (nor released) elsewhere.
  using OperationPromise = MozPromise<ResolveValueType, nsresult, true>;
  using OperationPromisePrivate = typename OperationPromise::Private;

  RefPtr<OperationPromisePrivate> operation =
      new OperationPromisePrivate(__func__);

  RefPtr<IPCActorUserGuard> guard = EnsureIPC();
  nsCOMPtr<nsIRunnable> runSession = NS_NewRunnableFunction(
      "SpeechRecognitionBackend::RunWithTransientSession",
      [operation, guard = std::move(guard), languages = std::move(aLanguages),
       aSendFunc]() mutable {
        AssertOnIPCThread();

        RefPtr<hwinference::HWInferenceManagerChild> manager =
            hwinference::HWInferenceManagerChild::GetSingleton();
        RefPtr<hwinference::SpeechRecognitionChild> child =
            manager ? manager->CreateSpeechRecognitionSession() : nullptr;
        if (!child) {
          operation->Reject(NS_ERROR_FAILURE, __func__);
          return;
        }
        // The connection is held open for exactly as long as this transient
        // session exists.
        child->SetIPCActorUserGuard(std::move(guard));

        aSendFunc(child, languages)
            ->Then(
                GetCurrentSerialEventTarget(), __func__,
                [operation,
                 child](typename SendPromise::ResolveOrRejectValue&& aValue) {
                  AssertOnIPCThread();
                  if (child->CanSend()) {
                    hwinference::SpeechRecognitionChild::Send__delete__(child);
                  }
                  if (aValue.IsReject()) {
                    operation->Reject(NS_ERROR_FAILURE, __func__);
                    return;
                  }
                  operation->Resolve(std::move(aValue.ResolveValue()),
                                     __func__);
                });
      });
  sIPCCapability->Dispatch(runSession.forget());

  operation->Then(
      GetMainThreadSerialEventTarget(), __func__,
      [aOnResult](ResolveValueType aResult) mutable {
        aOnResult(std::move(aResult));
      },
      [aOnFailure](nsresult aError) { aOnFailure(); });
}

/* static */
already_AddRefed<Promise> SpeechRecognitionBackend::Available(
    nsIGlobalObject* aGlobal, const nsTArray<nsCString>& aLanguages) {
  AssertIsOnMainThread();

  if (!aGlobal) {
    return nullptr;
  }

  ErrorResult rv;
  RefPtr<Promise> promise = Promise::Create(aGlobal, rv);
  if (rv.Failed()) {
    return nullptr;
  }

  nsTArray<nsCString> languages = aLanguages.Clone();
  if (languages.IsEmpty()) {
    languages.AppendElement("en-US"_ns);
  }

  LOG("SpeechRecognitionBackend::Available - Starting availability check for "
      "{} languages",
      languages.Length());

  if (MOZ_LOG_TEST(gSpeechRecognitionBackendLog, LogLevel::Debug)) {
    for (const auto& lang : languages) {
      LOG("SpeechRecognitionBackend::Available - Language requested: {}",
          lang.get());
    }
  }

  // https://webaudio.github.io/web-speech-api/#availability-algorithm
  // step 5.2.2: "available" if installed, "downloadable" if supported by
  // the user agent but not yet installed, "unavailable" if not supported.
  // IsModelAvailable alone can't tell "installed" from "not installed but
  // fetchable" apart (ModelHub.isModelAvailable returns true for both), so
  // check IsModelInstalled first for "available"; IsModelAvailable then
  // distinguishes "downloadable" from "unavailable".
  RefPtr<GenericPromise> installed = IsModelInstalledNative(languages.Clone());
  installed->Then(
      GetMainThreadSerialEventTarget(), __func__,
      [promise, languages = std::move(languages)](
          const GenericPromise::ResolveOrRejectValue& aValue) mutable {
        AssertIsOnMainThread();
        if (aValue.IsResolve() && aValue.ResolveValue()) {
          LOG("SpeechRecognitionBackend::Available - model installed");
          promise->MaybeResolve(AvailabilityStatus::Available);
          return;
        }
        RunWithTransientSession(
            std::move(languages),
            [](hwinference::SpeechRecognitionChild* aChild,
               nsTArray<nsCString>& aLangs) {
              return aChild->SendIsModelAvailable(aLangs);
            },
            [promise](bool aAvailable) {
              LOG("SpeechRecognitionBackend::Available - IsModelAvailable: {}",
                  aAvailable ? "true" : "false");
              promise->MaybeResolve(aAvailable
                                        ? AvailabilityStatus::Downloadable
                                        : AvailabilityStatus::Unavailable);
            },
            [promise]() {
              promise->MaybeResolve(AvailabilityStatus::Unavailable);
            });
      });

  return promise.forget();
}

/* static */
RefPtr<GenericPromise> SpeechRecognitionBackend::IsModelInstalledNative(
    nsTArray<nsCString>&& aLanguages) {
  AssertIsOnMainThread();

  LOG("SpeechRecognitionBackend::IsModelInstalledNative - Starting installed "
      "check for {} languages",
      aLanguages.Length());

  RefPtr<GenericPromise::Private> resultPromise =
      new GenericPromise::Private(__func__);

  RunWithTransientSession(
      std::move(aLanguages),
      [](hwinference::SpeechRecognitionChild* aChild,
         nsTArray<nsCString>& aLangs) {
        return aChild->SendIsModelInstalled(aLangs);
      },
      [resultPromise](bool aInstalled) {
        LOG("SpeechRecognitionBackend::IsModelInstalledNative - Received "
            "response: {}",
            aInstalled ? "true" : "false");
        resultPromise->Resolve(aInstalled, __func__);
      },
      [resultPromise]() { resultPromise->Resolve(false, __func__); });

  return resultPromise;
}

/* static */
void SpeechRecognitionBackend::EnsureConnectedOnIPCThread() {
  AssertOnIPCThread();

  RefPtr<mozilla::hwinference::HWInferenceManagerChild> child =
      mozilla::hwinference::HWInferenceManagerChild::GetSingleton();
  if (child && child->CanSend()) {
    return;
  }

  // Binding the child endpoint here makes the connection immediately usable;
  // if the utility process cannot be launched, the parent endpoint is dropped
  // and the actor is simply destroyed asynchronously. Messages sent in the
  // meantime are queued by the channel.
  mozilla::ipc::Endpoint<hwinference::PHWInferenceManagerParent> parentEp;
  mozilla::ipc::Endpoint<hwinference::PHWInferenceManagerChild> childEp;
  MOZ_ALWAYS_SUCCEEDS(
      hwinference::PHWInferenceManager::CreateEndpoints(&parentEp, &childEp));
  mozilla::hwinference::HWInferenceManagerChild::OpenForProcess(
      std::move(childEp));

  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "SpeechRecognitionBackend::RequestHWInferenceConnection",
      [parentEp = std::move(parentEp)]() mutable {
        // Null in the parent process, which never connects this way.
        if (ContentChild* contentChild = ContentChild::GetSingleton()) {
          contentChild->SendRequestHWInferenceConnection(std::move(parentEp));
        }
      }));
}

/* static */
already_AddRefed<IPCActorUserGuard> SpeechRecognitionBackend::EnsureIPC() {
  AssertIsOnMainThread();

  // Acquired before dispatching so that nothing can close the connection
  // between the setup below and the caller's use of it.
  RefPtr<IPCActorUserGuard> ipcActorUserGuard = new IPCActorUserGuard();

  nsCOMPtr<nsISerialEventTarget> ipcThread = GetOrCreateIPCThread();
  ipcThread->Dispatch(NS_NewRunnableFunction(
      "SpeechRecognitionBackend::EnsureConnectedOnIPCThread",
      [] { EnsureConnectedOnIPCThread(); }));

  return ipcActorUserGuard.forget();
}

/* static */
already_AddRefed<Promise> SpeechRecognitionBackend::Install(
    nsIGlobalObject* aGlobal, const nsTArray<nsCString>& aLanguages,
    uint64_t aInnerWindowId) {
  AssertIsOnMainThread();

  if (!aGlobal) {
    return nullptr;
  }

  ErrorResult rv;
  RefPtr<Promise> promise = Promise::Create(aGlobal, rv);
  if (rv.Failed()) {
    return nullptr;
  }

  if (aLanguages.IsEmpty()) {
    promise->MaybeResolve(false);
    return promise.forget();
  }

  nsTArray<nsCString> languages = aLanguages.Clone();

  LOG("SpeechRecognitionBackend::Install - Starting install for {} languages",
      languages.Length());

  RunWithTransientSession(
      std::move(languages),
      [aInnerWindowId](hwinference::SpeechRecognitionChild* aChild,
                       nsTArray<nsCString>& aLangs) {
        return aChild->SendInstallModels(std::move(aLangs), aInnerWindowId);
      },
      [promise](bool aSuccess) {
        LOG("SpeechRecognitionBackend::Install - Install completed: {}",
            aSuccess ? "success" : "failed");
        promise->MaybeResolve(aSuccess);
      },
      [promise]() { promise->MaybeResolve(false); });

  return promise.forget();
}

}  // namespace mozilla::dom

#undef LOG
#undef LOGV
#undef LOGE
