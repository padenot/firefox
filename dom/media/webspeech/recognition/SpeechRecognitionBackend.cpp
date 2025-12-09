/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2  et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionBackend.h"

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
#include "mozilla/ipc/MessageChannel.h"
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/SpeechRecognitionChild.h"

namespace mozilla::dom {

using namespace mozilla::ipc;

StaticRefPtr<nsIThread> SpeechRecognitionBackend::sIPCThread;
mozilla::EventTargetCapability<nsIThread>*
    SpeechRecognitionBackend::sIPCCapability = nullptr;
IPCThreadUserCounter SpeechRecognitionBackend::sIPCThreadUsers;
StaticRefPtr<mozilla::hwinference::HWInferenceManagerChild>
    SpeechRecognitionBackend::sHWInferenceChild;

void IPCThreadUserCounter::Increment() { mCount++; }

void IPCThreadUserCounter::Decrement() {
  MOZ_ASSERT(mCount > 0);
  mCount--;
  if (!mCount) {
    SpeechRecognitionBackend::StopIPCThreadIfPossible();
  }
}

bool IPCThreadUserCounter::IsZero() const { return !mCount; }

/* static */
void SpeechRecognitionBackend::CloseHWInferenceChildIfAny() {
  AssertOnIPCThread();
  if (sHWInferenceChild) {
    sHWInferenceChild->Close();
    sHWInferenceChild = nullptr;
  }
}

NS_IMPL_ISUPPORTS(SpeechRecognitionBackend::ShutdownTask, nsITargetShutdownTask)

void SpeechRecognitionBackend::ShutdownTask::TargetShutdown() {
  SpeechRecognitionBackend::CloseHWInferenceChildIfAny();
}

TransientSpeechRecognitionSession::TransientSpeechRecognitionSession()
    : mChild(SpeechRecognitionBackend::sHWInferenceChild
                 ->CreateSpeechRecognitionSession()) {}

TransientSpeechRecognitionSession::~TransientSpeechRecognitionSession() {
  SpeechRecognitionBackend::AssertOnIPCThread();
  if (mChild) {
    SpeechRecognitionChild::Send__delete__(mChild);
    mChild = nullptr;
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

SpeechRecognitionBackend::IPCThreadUserGuard::~IPCThreadUserGuard() {
  if (NS_IsMainThread()) {
    SpeechRecognitionBackend::ReleaseIPCThreadUser();
  } else {
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "IPCThreadUserGuard::Release",
        [] { SpeechRecognitionBackend::ReleaseIPCThreadUser(); }));
  }
}

SpeechRecognitionBackend::SpeechRecognitionBackend(
    SpeechRecognition* aParent, uint32_t aGraphRate, const nsString& aLanguage,
    const nsTArray<nsString>& aPhrases)
    : mParent(aParent),
      mLanguage(NS_ConvertUTF16toUTF8(aLanguage)),
      mPhrases(aPhrases.Clone()) {}

SpeechRecognitionBackend::~SpeechRecognitionBackend() {
  Abort();
}

nsresult SpeechRecognitionBackend::Start() {
  return NS_OK;
}

void SpeechRecognitionBackend::Stop() {
}

void SpeechRecognitionBackend::Abort() {
  AssertIsOnMainThread();
  LOG("SpeechRecognitionBackend::Abort");
  Stop();
}

void SpeechRecognitionBackend::AttachToTrack(AudioStreamTrack* aTrack) {
  AssertIsOnMainThread();
  MOZ_ASSERT(aTrack);
}

void SpeechRecognitionBackend::DetachFromTrack() {
  AssertIsOnMainThread();
}

void SpeechRecognitionBackend::DataCallback(TrackTime aTime,
                                            const AudioChunk& aChunk) {}

void SpeechRecognitionBackend::NotifyTrackEnded() {}

/* static */
nsCOMPtr<nsIThread> SpeechRecognitionBackend::GetOrCreateIPCThread() {
  AssertIsOnMainThread();

  if (!sIPCThread) {
    nsCOMPtr<nsIThread> thread;
    nsresult rv = NS_NewNamedThread("SpeechIPC", getter_AddRefs(thread));
    if (NS_SUCCEEDED(rv)) {
      sIPCThread = thread;
      sIPCCapability = new EventTargetCapability<nsIThread>(sIPCThread);
      LOG("Created shared IPC thread for speech recognition");
      // The thread and the HWInference connection it carries are kept alive
      // for the lifetime of the content process (see StopIPCThreadIfPossible)
      // rather than being torn down and recreated opportunistically. Actors
      // are bound to the specific nsIThread instance they were opened on;
      // tearing down and recreating this thread while an actor referencing
      // the old instance could still be in use (e.g. a concurrent EnsureIPC()
      // caller) led to actors being used from the wrong thread and crashing.
      //
      // The actor is closed via a shutdown task registered on the thread
      // itself, which runs on the IPC thread as part of its own shutdown
      // sequence, rather than a runnable dispatched from RunOnShutdown:
      // dispatching to a thread whose queue is already closing can fail, and
      // Gecko's infallible-dispatch failure path (MaybeLeakRefPtr) leaks the
      // runnable instead of running it, silently dropping the close.
      RefPtr<ShutdownTask> shutdownTask = MakeRefPtr<ShutdownTask>();
      MOZ_ALWAYS_SUCCEEDS(sIPCThread->RegisterShutdownTask(shutdownTask));

      RunOnShutdown([] {
        AssertIsOnMainThread();
        if (sIPCThread) {
          nsCOMPtr<nsIThread> thread = sIPCThread.forget();
          delete sIPCCapability;
          sIPCCapability = nullptr;
          thread->Shutdown();
        }
      });
    } else {
      LOG("Failed to create shared IPC thread");
      return nullptr;
    }
  }

  return nsCOMPtr<nsIThread>(sIPCThread);
}

void SpeechRecognitionBackend::StopIPCThreadIfPossible() {
  AssertIsOnMainThread();
  // The shared IPC thread and its HWInference connection are kept alive for
  // the process lifetime (see GetOrCreateIPCThread); nothing to do here once
  // there are no more users. Close the actor when idle so the parent-side
  // connection doesn't linger, but keep the same thread instance around so a
  // later EnsureIPC() call never has to reason about a stale actor bound to a
  // different, dead thread.
  if (sIPCThreadUsers.IsZero() && sIPCThread) {
    // Unlike GetOrCreateIPCThread's ShutdownTask, this doesn't run as part of
    // the thread's own shutdown - it's a live idle-close while the thread
    // keeps running - so it's still a plain dispatch. Pass NS_DISPATCH_FALLIBLE
    // so a Dispatch() that loses a race against real process shutdown releases
    // the runnable normally instead of MaybeLeakRefPtr leaking it; ShutdownTask
    // will close the actor for real once the thread does shut down.
    sIPCThread->Dispatch(
        NS_NewRunnableFunction(
            "SpeechRecognitionBackend::CloseHWInferenceChildWhenIdle",
            [] { CloseHWInferenceChildIfAny(); }),
        NS_DISPATCH_FALLIBLE);
  }
}

/* static */
void SpeechRecognitionBackend::AcquireIPCThreadUser() {
  AssertIsOnMainThread();
  sIPCThreadUsers.Increment();
}

/* static */
void SpeechRecognitionBackend::ReleaseIPCThreadUser() {
  AssertIsOnMainThread();
  sIPCThreadUsers.Decrement();
}

/* static */
void SpeechRecognitionBackend::AssertOnIPCThread() {
  sIPCCapability->AssertOnCurrentThread();
}

/* static */
template <typename Func>
void SpeechRecognitionBackend::OnIPCThread(Func&& aFunc) {
  MOZ_ASSERT(sIPCThread, "Programming error: IPC thread not initialized");
  sIPCThread->Dispatch(NS_NewRunnableFunction(
      "SpeechRecognitionBackend::OnIPCThread", std::forward<Func>(aFunc)));
}

/* static */
template <typename SendFunc, typename OnResult, typename OnFailure>
void SpeechRecognitionBackend::RunWithTransientSession(
    nsTArray<nsCString>&& aLanguages, SendFunc aSendFunc, OnResult aOnResult,
    OnFailure aOnFailure) {
  AssertIsOnMainThread();

  // ipcUserGuard holds the shared IPC thread alive for the whole operation,
  // releasing when the operation's async chain finishes - tied to its own
  // lifetime, not to the promise settling (see IPCThreadUserGuard).
  auto [ready, ipcUserGuard] = EnsureIPC();
  ready->Then(
      GetCurrentSerialEventTarget(), __func__,
      [ipcUserGuard, languages = std::move(aLanguages), aSendFunc, aOnResult,
       aOnFailure](bool aSuccess) mutable {
        AssertIsOnMainThread();
        if (!aSuccess) {
          aOnFailure();
          return;
        }

        OnIPCThread([ipcUserGuard, languages = std::move(languages), aSendFunc,
                     aOnResult, aOnFailure]() mutable {
          auto session = MakeUnique<TransientSpeechRecognitionSession>();
          if (!session->get()) {
            NS_DispatchToMainThread(NS_NewRunnableFunction(
                "SpeechRecognitionBackend::RunWithTransientSession::"
                "NoSession",
                [ipcUserGuard, aOnFailure]() { aOnFailure(); }));
            return;
          }

          using ResolveValueType = typename std::remove_pointer_t<
              decltype(aSendFunc(session->get(), languages)
                           .get())>::ResolveValueType;
          aSendFunc(session->get(), languages)
              ->Then(
                  GetCurrentSerialEventTarget(), __func__,
                  [ipcUserGuard, session = std::move(session),
                   aOnResult](ResolveValueType aResult) {
                    NS_DispatchToMainThread(NS_NewRunnableFunction(
                        "SpeechRecognitionBackend::RunWithTransientSession::"
                        "Resolve",
                        [ipcUserGuard, aOnResult, aResult]() {
                          aOnResult(aResult);
                        }));
                  },
                  [ipcUserGuard, session = std::move(session),
                   aOnFailure](ResponseRejectReason aReason) {
                    NS_DispatchToMainThread(NS_NewRunnableFunction(
                        "SpeechRecognitionBackend::RunWithTransientSession::"
                        "Reject",
                        [ipcUserGuard, aOnFailure]() { aOnFailure(); }));
                  });
        });
      },
      [ipcUserGuard, aOnFailure](nsresult aError) { aOnFailure(); });
}

/* static */
already_AddRefed<Promise> SpeechRecognitionBackend::Available(
    nsIGlobalObject* aGlobal, const nsTArray<nsString>& aLanguages) {
  AssertIsOnMainThread();

  if (!aGlobal) {
    return nullptr;
  }

  ErrorResult rv;
  RefPtr<Promise> promise = Promise::Create(aGlobal, rv);
  if (rv.Failed()) {
    return nullptr;
  }

  nsTArray<nsCString> languages;
  for (const nsString& lang : aLanguages) {
    languages.AppendElement(NS_ConvertUTF16toUTF8(lang));
  }
  if (languages.IsEmpty()) {
    languages.AppendElement("en-US"_ns);
  }

  LOG("SpeechRecognitionBackend::Available - Starting availability check for "
      "{} languages",
      languages.Length());

  for (const auto& lang : languages) {
    LOG("SpeechRecognitionBackend::Available - Language requested: {}",
        lang.get());
  }

  nsMainThreadPtrHandle<Promise> promiseHandle(
      MakeAndAddRef<nsMainThreadPtrHolder<Promise>>(
          "SpeechRecognitionBackend::Available", promise));

  RunWithTransientSession(
      std::move(languages),
      [](SpeechRecognitionChild* aChild, nsTArray<nsCString>& aLangs) {
        return aChild->SendIsModelAvailable(aLangs);
      },
      [promiseHandle](bool aAvailable) {
        LOG("SpeechRecognitionBackend::Available - Received response: {}",
            aAvailable ? "true" : "false");
        promiseHandle->MaybeResolve(aAvailable
                                        ? AvailabilityStatus::Available
                                        : AvailabilityStatus::Downloadable);
      },
      [promiseHandle]() {
        promiseHandle->MaybeResolve(AvailabilityStatus::Unavailable);
      });

  return promise.forget();
}

/* static */
/* static */
SpeechRecognitionBackend::EnsureIPCResult
SpeechRecognitionBackend::EnsureIPC() {
  AssertIsOnMainThread();

  // This ensures IPC has been setup. It requires two things:
  //
  // - a thread, on which all IPC calls are made
  // - effectively setting up the connection.
  //
  // Since sHWInferenceChild is SpeechIPC thread only, and this isn't
  // performance sensitive by any mean, we do this asynchronously. If not
  // set-up, the connection is established (on the main thread), then
  // sHWInferenceChild is initialized, and the promise is finally resolved.

  nsCOMPtr<nsIThread> ipcThread = GetOrCreateIPCThread();
  if (!ipcThread) {
    LOG("EnsureIPC - Failed to get IPC thread");
    return {
        mozilla::GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__),
        nullptr};
  }

  // Acquired before the first await point so that the thread can't be
  // idle-closed while this connection attempt (or the caller's use of it
  // afterwards) is still in flight.
  RefPtr<IPCThreadUserGuard> ipcUserGuard = new IPCThreadUserGuard();

  RefPtr<GenericPromise> ready =
      InvokeAsync(ipcThread, __func__, []() -> RefPtr<GenericPromise> {
        AssertOnIPCThread();
        if (sHWInferenceChild && sHWInferenceChild->CanSend()) {
          LOG("EnsureIPC - Connection already established (checked on IPC "
              "thread)");
          return GenericPromise::CreateAndResolve(true, __func__);
        }

        return InvokeAsync(
            GetMainThreadSerialEventTarget(), __func__,
            []() -> RefPtr<GenericPromise> {
              AssertIsOnMainThread();

              ContentChild* contentChild = ContentChild::GetSingleton();
              if (!contentChild) {
                LOG("EnsureIPC - No ContentChild available");
                return GenericPromise::CreateAndReject(NS_ERROR_FAILURE,
                                                       __func__);
              }

              LOG("EnsureIPC - Creating endpoint pair");

              mozilla::ipc::Endpoint<hwinference::PHWInferenceManagerParent>
                  parentEp;
              mozilla::ipc::Endpoint<hwinference::PHWInferenceManagerChild>
                  childEp;

              MOZ_ALWAYS_SUCCEEDS(
                  hwinference::PHWInferenceManager::CreateEndpoints(&parentEp,
                                                                    &childEp));

              // Wait for the parent process to confirm the utility process
              // has actually launched and accepted the parent endpoint before
              // opening our side and calling the connection ready: otherwise
              // a launch/accept failure on the other end would go unnoticed
              // and later API calls would fail indirectly instead.
              return contentChild
                  ->SendRequestHWInferenceConnection(std::move(parentEp))
                  ->Then(
                      GetMainThreadSerialEventTarget(), __func__,
                      [childEp = std::move(childEp)](
                          bool aAccepted) mutable -> RefPtr<GenericPromise> {
                        if (!aAccepted) {
                          LOG("EnsureIPC - Utility process failed to accept "
                              "the HWInference connection");
                          return GenericPromise::CreateAndReject(
                              NS_ERROR_FAILURE, __func__);
                        }
                        return InvokeAsync(
                            sIPCThread, __func__,
                            [childEp = std::move(
                                 childEp)]() mutable -> RefPtr<GenericPromise> {
                              AssertOnIPCThread();

                              LOG("EnsureIPC - Opening connection on the "
                                  "SpeechIPC thread");
                              [&]() MOZ_NO_THREAD_SAFETY_ANALYSIS {
                                mozilla::hwinference::HWInferenceManagerChild::
                                    OpenForProcess(std::move(childEp));
                              }();
                              sHWInferenceChild = mozilla::hwinference::
                                  HWInferenceManagerChild::GetSingleton();

                              if (sHWInferenceChild->CanSend()) {
                                LOG("EnsureIPC - Connection established");
                                return GenericPromise::CreateAndResolve(
                                    true, __func__);
                              }
                              LOG("EnsureIPC - Failed to establish");
                              return GenericPromise::CreateAndReject(
                                  NS_ERROR_FAILURE, __func__);
                            });
                      },
                      [](mozilla::ipc::ResponseRejectReason aReason) {
                        LOG("EnsureIPC - RequestHWInferenceConnection IPC "
                            "call failed");
                        return GenericPromise::CreateAndReject(NS_ERROR_FAILURE,
                                                               __func__);
                      });
            });
      });

  return {std::move(ready), std::move(ipcUserGuard)};
}

/* static */
already_AddRefed<Promise> SpeechRecognitionBackend::Install(
    nsIGlobalObject* aGlobal, const nsTArray<nsString>& aLanguages) {
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

  nsTArray<nsCString> languages;
  for (const nsString& lang : aLanguages) {
    languages.AppendElement(NS_ConvertUTF16toUTF8(lang));
  }

  LOG("SpeechRecognitionBackend::Install - Starting install for {} languages",
      languages.Length());

  nsMainThreadPtrHandle<Promise> promiseHandle(
      MakeAndAddRef<nsMainThreadPtrHolder<Promise>>(
          "SpeechRecognitionBackend::Install", promise));

  RunWithTransientSession(
      std::move(languages),
      [](SpeechRecognitionChild* aChild, nsTArray<nsCString>& aLangs) {
        return aChild->SendInstallModels(std::move(aLangs));
      },
      [promiseHandle](bool aSuccess) {
        LOG("SpeechRecognitionBackend::Install - Install completed: {}",
            aSuccess ? "success" : "failed");
        promiseHandle->MaybeResolve(aSuccess);
      },
      [promiseHandle]() { promiseHandle->MaybeResolve(false); });

  return promise.forget();
}

}  // namespace mozilla::dom

#undef LOG
#undef LOGV
#undef LOGE
