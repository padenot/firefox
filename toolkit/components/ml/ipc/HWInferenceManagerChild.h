/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef TOOLKIT_COMPONENTS_ML_IPC_HWINFERENCEMANAGERCHILD_H_
#define TOOLKIT_COMPONENTS_ML_IPC_HWINFERENCEMANAGERCHILD_H_

#include "mozilla/hwinference/PHWInferenceManagerChild.h"
#include "mozilla/hwinference/SpeechRecognitionChild.h"
#include "nsRefPtrHashtable.h"
#include "MainThreadUtils.h"
#include "mozilla/MozPromise.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/ThreadSafety.h"
#include "mozilla/EventTargetCapability.h"

namespace mozilla::hwinference {

class HWInferenceManagerChild;

// Holds the process-wide PHWInferenceManager connection open for as long as it
// lives: the connection is established when the first guard is taken, and
// closed when the last one goes away. Consumers decide for themselves how long
// they hold a guard - one that wants the connection to survive a gap between
// two uses simply keeps its guard across that gap.
class HWInferenceConnectionGuard final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(HWInferenceConnectionGuard)

 private:
  friend class HWInferenceManagerChild;
  HWInferenceConnectionGuard();
  ~HWInferenceConnectionGuard();
};

// Resolves with a SpeechRecognitionChild already bound on the event target
// that was passed to CreateSpeechRecognitionSession(), and on which the
// promise is resolved.
using SpeechRecognitionSessionPromise =
    MozPromise<RefPtr<SpeechRecognitionChild>, nsresult, true>;

// Content process side. Process-wide singleton, bound on the main thread: it
// is shared by every HWInference consumer, so no single consumer gets to pick
// the thread it runs on. Task actors are separate toplevel connections created
// over it, and each consumer binds its side wherever it wants.
class HWInferenceManagerChild final : public PHWInferenceManagerChild {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(HWInferenceManagerChild, override);

  HWInferenceManagerChild() = default;

  // Adopts aEndpoint as the singleton connection, and returns whether it did.
  static bool OpenForProcess(Endpoint<PHWInferenceManagerChild>&& aEndpoint);

  // Establishes the shared connection if it isn't up yet, and returns a guard
  // keeping it open: GetSingleton() is usable as soon as this returns. Main
  // thread only.
  static already_AddRefed<HWInferenceConnectionGuard> AcquireConnection();

  // Reconnects if the connection died, e.g. because the utility process
  // crashed while a consumer was holding a guard. Cheap when it is still up.
  // Main thread only.
  static void EnsureConnected();

  static RefPtr<HWInferenceManagerChild> GetSingleton();

  void ActorDestroy(ActorDestroyReason aReason) override;

  // Asks the utility process for a new toplevel PSpeechRecognition connection
  // and binds this process' side of it on aTarget. Main thread only.
  RefPtr<SpeechRecognitionSessionPromise> CreateSpeechRecognitionSession(
      nsISerialEventTarget* aTarget);

 private:
  friend class HWInferenceConnectionGuard;

  ~HWInferenceManagerChild() = default;

  static void ReleaseConnectionReference();

  static void AcquireConnectionUser();
  static void ReleaseConnectionUser();

  static StaticRefPtr<HWInferenceManagerChild> sSingleton
      MOZ_GUARDED_BY(sMainThreadCapability);
  // Number of live HWInferenceConnectionGuards, across all consumers.
  static uint32_t sConnectionUsers MOZ_GUARDED_BY(sMainThreadCapability);
};

}  // namespace mozilla::hwinference

#endif  // TOOLKIT_COMPONENTS_ML_IPC_HWINFERENCEMANAGERCHILD_H_
