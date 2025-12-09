/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2  et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognitionBackend.h"

#include "SpeechRecognition.h"
#include "SpeechTrackListener.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/Assertions.h"
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

TransientSpeechRecognitionSession::TransientSpeechRecognitionSession()
    : mChild(SpeechRecognitionBackend::sHWInferenceChild
                 ->CreateSpeechRecognitionSession()) {
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "TransientSpeechRecognitionSession::Increment", []() {
        AssertIsOnMainThread();
        SpeechRecognitionBackend::sIPCThreadUsers.Increment();
      }));
}

TransientSpeechRecognitionSession::~TransientSpeechRecognitionSession() {
  SpeechRecognitionBackend::AssertOnIPCThread();
  if (mChild) {
    SpeechRecognitionChild::Send__delete__(mChild);
    mChild = nullptr;
  }
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "TransientSpeechRecognitionSession::Decrement", []() {
        AssertIsOnMainThread();
        SpeechRecognitionBackend::sIPCThreadUsers.Decrement();
      }));
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
    } else {
      LOG("Failed to create shared IPC thread");
      return nullptr;
    }
  }

  return nsCOMPtr<nsIThread>(sIPCThread);
}

void SpeechRecognitionBackend::StopIPCThreadIfPossible() {
  AssertIsOnMainThread();
  if (sIPCThreadUsers.IsZero()) {
    nsCOMPtr<nsIThread> ipcThread = sIPCThread.forget();
    delete sIPCCapability;
    sIPCCapability = nullptr;
    ipcThread->Shutdown();
    LOG("Stopped shared IPC thread");
  }
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

  EnsureIPC()->Then(
      GetCurrentSerialEventTarget(), __func__,
      [promiseHandle, languages = std::move(languages)](bool aSuccess) mutable {
        AssertIsOnMainThread();
        if (!aSuccess) {
          LOG("SpeechRecognitionBackend::Available - Failed to initialize IPC");
          promiseHandle->MaybeResolve(AvailabilityStatus::Unavailable);
          return;
        }

        OnIPCThread([promiseHandle,
                     languages = std::move(languages)]() mutable {
          LOG("SpeechRecognitionBackend::Available - Connection ready, "
              "checking model availability");

          auto session = MakeUnique<TransientSpeechRecognitionSession>();

          if (!session->get()) {
            LOG("SpeechRecognitionBackend::Available - Failed to create speech "
                "recognition session");
            NS_DispatchToMainThread(NS_NewRunnableFunction(
                "SpeechRecognitionBackend::ResolveUnavailable",
                [promiseHandle]() {
                  promiseHandle->MaybeResolve(AvailabilityStatus::Unavailable);
                }));
            return;
          }

          session->get()->SendIsModelAvailable(languages)->Then(
              GetCurrentSerialEventTarget(), __func__,
              [promiseHandle, session = std::move(session)](bool available) {
                LOG("SpeechRecognitionBackend::Available - Received response: "
                    "{}",
                    available ? "true" : "false");
                NS_DispatchToMainThread(NS_NewRunnableFunction(
                    "SpeechRecognitionBackend::ResolveAvailable",
                    [promiseHandle, available]() {
                      if (available) {
                        promiseHandle->MaybeResolve(
                            AvailabilityStatus::Available);
                      } else {
                        promiseHandle->MaybeResolve(
                            AvailabilityStatus::Downloadable);
                      }
                    }));
              },
              [promiseHandle,
               session = std::move(session)](ResponseRejectReason reason) {
                LOG("SpeechRecognitionBackend::Available failed: {}",
                    static_cast<int>(reason));
                NS_DispatchToMainThread(NS_NewRunnableFunction(
                    "SpeechRecognitionBackend::ResolveUnavailable",
                    [promiseHandle]() {
                      promiseHandle->MaybeResolve(
                          AvailabilityStatus::Unavailable);
                    }));
              });
        });
      },
      [promiseHandle](nsresult aError) {
        LOG("SpeechRecognitionBackend::Available - IPC initialization failed");
        promiseHandle->MaybeResolve(AvailabilityStatus::Unavailable);
      });

  return promise.forget();
}

/* static */
RefPtr<GenericPromise> SpeechRecognitionBackend::EnsureIPC() {
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
    return mozilla::GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
  }

  return InvokeAsync(ipcThread, __func__, []() -> RefPtr<GenericPromise> {
    AssertOnIPCThread();
    if (sHWInferenceChild && sHWInferenceChild->CanSend()) {
      LOG("EnsureIPC - Connection already established (checked on IPC thread)");
      return GenericPromise::CreateAndResolve(true, __func__);
    }

    return InvokeAsync(
        GetMainThreadSerialEventTarget(), __func__,
        []() -> RefPtr<GenericPromise> {
          AssertIsOnMainThread();

          ContentChild* contentChild = ContentChild::GetSingleton();
          if (!contentChild) {
            LOG("EnsureIPC - No ContentChild available");
            return GenericPromise::CreateAndReject(NS_ERROR_FAILURE, __func__);
          }

          LOG("EnsureIPC - Creating endpoint pair");

          mozilla::ipc::Endpoint<hwinference::PHWInferenceManagerParent> parentEp;
          mozilla::ipc::Endpoint<hwinference::PHWInferenceManagerChild> childEp;

          MOZ_ALWAYS_SUCCEEDS(
              hwinference::PHWInferenceManager::CreateEndpoints(&parentEp, &childEp));
          DebugOnly<bool> ok = contentChild->SendRequestHWInferenceConnection(
              std::move(parentEp));
          MOZ_ASSERT(ok);

          return InvokeAsync(
              sIPCThread, __func__,
              [childEp =
                   std::move(childEp)]() mutable -> RefPtr<GenericPromise> {
                AssertOnIPCThread();

                LOG("EnsureIPC - Opening connection on the SpeechIPC thread");
                [&]() MOZ_NO_THREAD_SAFETY_ANALYSIS {
                  mozilla::hwinference::HWInferenceManagerChild::OpenForProcess(std::move(childEp));
                }();
                sHWInferenceChild = mozilla::hwinference::HWInferenceManagerChild::GetSingleton();

                if (sHWInferenceChild->CanSend()) {
                  LOG("EnsureIPC - Connection established");
                  return GenericPromise::CreateAndResolve(true, __func__);
                }
                LOG("EnsureIPC - Failed to establish");
                return GenericPromise::CreateAndReject(NS_ERROR_FAILURE,
                                                       __func__);
              });
        });
  });
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

  EnsureIPC()->Then(
      GetCurrentSerialEventTarget(), __func__,
      [promiseHandle, languages = std::move(languages)](bool aSuccess) mutable {
        if (!aSuccess) {
          LOG("SpeechRecognitionBackend::Install - Failed to initialize IPC");
          promiseHandle->MaybeResolve(false);
          return;
        }

        OnIPCThread([promiseHandle,
                     languages = std::move(languages)]() mutable {
          LOG("SpeechRecognitionBackend::Install - Connection ready, starting "
              "model installation");

          auto session = MakeUnique<TransientSpeechRecognitionSession>();

          session->get()
              ->SendInstallModels(std::move(languages))
              ->Then(
                  GetCurrentSerialEventTarget(), __func__,
                  [promiseHandle, session = std::move(session)](bool success) {
                    LOG("SpeechRecognitionBackend::Install - Install "
                        "completed: {}",
                        success ? "success" : "failed");
                    NS_DispatchToMainThread(NS_NewRunnableFunction(
                        "SpeechRecognitionBackend::ResolveInstall",
                        [promiseHandle, success]() {
                          promiseHandle->MaybeResolve(success);
                        }));
                  },
                  [promiseHandle,
                   session = std::move(session)](ResponseRejectReason aReason) {
                    LOG("SpeechRecognitionBackend::Install - Install failed "
                        "with reason: {}",
                        static_cast<int>(aReason));
                    NS_DispatchToMainThread(NS_NewRunnableFunction(
                        "SpeechRecognitionBackend::ResolveInstallFailed",
                        [promiseHandle]() {
                          promiseHandle->MaybeResolve(false);
                        }));
                  });
        });
      },
      [promiseHandle](nsresult aError) {
        LOG("SpeechRecognitionBackend::Install - IPC initialization failed");
        promiseHandle->MaybeResolve(false);
      });

  return promise.forget();
}

}  // namespace mozilla::dom

#undef LOG
#undef LOGV
#undef LOGE
