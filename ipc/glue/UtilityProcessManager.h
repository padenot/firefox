/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef _include_ipc_glue_UtilityProcessManager_h_
#define _include_ipc_glue_UtilityProcessManager_h_
#include "mozilla/MozPromise.h"
#include "mozilla/dom/ipc/IdType.h"
#include "mozilla/ipc/UtilityProcessHost.h"
#ifndef ANDROID
#  include "mozilla/hwinference/HWInferenceParent.h"
#  include "mozilla/hwinference/PHWInferenceManagerChild.h"
#endif  // !ANDROID
#include "mozilla/ProcInfo.h"
#include "nsIObserver.h"
#include "nsTArray.h"

#include "mozilla/PRemoteMediaManagerChild.h"

#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
#  include "mozilla/psm/PKCS11ModuleParent.h"
#endif  // NIGHTLY_BUILD && !MOZ_NO_SMART_CARDS

namespace mozilla {

class MemoryReportingProcess;

namespace dom {
class JSOracleParent;
class WindowsUtilsParent;
}  // namespace dom

namespace widget::filedialog {
class ProcessProxy;
}  // namespace widget::filedialog

namespace ipc {

class UtilityProcessParent;
class UtilityProcessKeepAlive;

// The UtilityProcessManager is a singleton responsible for creating
// Utility-bound objects that may live in another process. Currently, it
// provides access to the Utility process via ContentParent.
class UtilityProcessManager final : public UtilityProcessHost::Listener {
  friend class UtilityProcessParent;
  friend class UtilityProcessKeepAlive;

 public:
  template <typename T>
  using LaunchPromise = MozPromise<T, LaunchError, true>;
  template <typename T>
  using SharedLaunchPromise = MozPromise<T, LaunchError, false>;

  using StartRemoteDecodingUtilityPromise =
      LaunchPromise<Endpoint<PRemoteMediaManagerChild>>;
  using JSOraclePromise = GenericNonExclusivePromise;

#ifdef XP_WIN
  using WindowsUtilsPromise = LaunchPromise<RefPtr<dom::WindowsUtilsParent>>;
  using WinFileDialogPromise = LaunchPromise<widget::filedialog::ProcessProxy>;
#endif

#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
  using PKCS11ModulePromise = LaunchPromise<RefPtr<psm::PKCS11ModuleParent>>;
#endif  // NIGHTLY_BUILD && !MOZ_NO_SMART_CARDS

#ifndef ANDROID
  using HWInferencePromise =
      LaunchPromise<RefPtr<hwinference::HWInferenceParent>>;
#endif  // !ANDROID

  static RefPtr<UtilityProcessManager> GetSingleton();

  static RefPtr<UtilityProcessManager> GetIfExists();

  // Launch a new Utility process asynchronously. aInstanceKey distinguishes
  // multiple concurrent processes of the same SandboxingKind (see
  // StartHWInference); pass the empty string for every SandboxingKind that
  // only ever has one instance.
  RefPtr<SharedLaunchPromise<Ok>> LaunchProcess(
      SandboxingKind aSandbox, const nsACString& aInstanceKey = ""_ns);

  // Like LaunchProcess(), but hands back a keep-alive on the process instance:
  // it is shut down as soon as the last keep-alive on it goes away, rather than
  // living until browser shutdown. There is a single keep-alive per instance,
  // shared by every caller, so a kind opts into this policy by having all of
  // its consumers come through here. Returns nullptr if the launch failed
  // outright. Main thread only.
  already_AddRefed<UtilityProcessKeepAlive> LaunchProcessWithKeepAlive(
      SandboxingKind aSandbox, const nsACString& aInstanceKey = ""_ns);

  template <typename Actor>
  RefPtr<LaunchPromise<Ok>> StartUtility(
      RefPtr<Actor> aActor, SandboxingKind aSandbox,
      const nsACString& aInstanceKey = ""_ns);

  RefPtr<StartRemoteDecodingUtilityPromise> StartProcessForRemoteMediaDecoding(
      EndpointProcInfo aOtherProcess, dom::ContentParentId aChildId,
      SandboxingKind aSandbox);

  RefPtr<JSOraclePromise> StartJSOracle(mozilla::dom::JSOracleParent* aParent);

#ifdef XP_WIN
  // Get the (possibly already resolved) promise for the Windows utility
  // process actor.  Creates the process if it is not running.
  RefPtr<WindowsUtilsPromise> GetWindowsUtilsPromise();
  // Releases the WindowsUtils actor so that it can be destroyed.
  // Subsequent attempts to use WindowsUtils will create a new process.
  void ReleaseWindowsUtils();

  // Get a new Windows file-dialog utility-process actor. These are never
  // reused; this will always return a fresh actor.
  RefPtr<WinFileDialogPromise> CreateWinFileDialogActor();
#endif

#if defined(NIGHTLY_BUILD) && !defined(MOZ_NO_SMART_CARDS)
  RefPtr<PKCS11ModulePromise> StartPKCS11Module();
#endif  // NIGHTLY_BUILD && !MOZ_NO_SMART_CARDS

#ifndef ANDROID
  // Starts (or reuses) the HWInference process instance for aInstanceKey
  // (see HWINFERENCE_*_INSTANCE_KEY in HWInferenceParent.h).
  RefPtr<HWInferencePromise> StartHWInference(const nsACString& aInstanceKey);

  // Starts the content HWInference instance and hands aEndpoint to it. The
  // endpoint closes itself if that fails, so there is nothing to report back.
  // The returned keep-alive is the caller's share of that instance's lifetime.
  already_AddRefed<UtilityProcessKeepAlive> StartContentHWInferenceManager(
      Endpoint<hwinference::PHWInferenceManagerParent>&& aEndpoint,
      dom::ContentParentId aChildId);
#endif  // !ANDROID

  void OnProcessUnexpectedShutdown(UtilityProcessHost* aHost);

  // Returns the platform pid for this utility sandbox process.
  Maybe<base::ProcessId> ProcessPid(SandboxingKind aSandbox,
                                    const nsACString& aInstanceKey = ""_ns);

  // Create a MemoryReportingProcess object for this utility process
  RefPtr<MemoryReportingProcess> GetProcessMemoryReporter(
      UtilityProcessParent* parent);

  // Returns access to the PUtility protocol if a Utility process for that
  // sandbox is present.
  RefPtr<UtilityProcessParent> GetProcessParent(
      SandboxingKind aSandbox, const nsACString& aInstanceKey = ""_ns) {
    RefPtr<ProcessFields> p = GetProcess(aSandbox, aInstanceKey);
    if (!p) {
      return nullptr;
    }
    return p->mProcessParent;
  }

  // Get a list of all valid utility process parent references
  nsTArray<RefPtr<UtilityProcessParent>> GetAllProcessesProcessParent() {
    nsTArray<RefPtr<UtilityProcessParent>> rv;
    for (auto& p : mProcesses) {
      if (p->mProcessParent) {
        rv.AppendElement(p->mProcessParent);
      }
    }
    return rv;
  }

  // Returns the Utility Process for that sandbox
  UtilityProcessHost* Process(SandboxingKind aSandbox,
                              const nsACString& aInstanceKey = ""_ns) {
    RefPtr<ProcessFields> p = GetProcess(aSandbox, aInstanceKey);
    if (!p) {
      return nullptr;
    }
    return p->mProcess;
  }

  void RegisterActor(const RefPtr<UtilityProcessParent>& aParent,
                     UtilityActorName aActorName) {
    for (auto& p : mProcesses) {
      if (p->mProcessParent && p->mProcessParent == aParent) {
        p->mActors.AppendElement(aActorName);
        return;
      }
    }
  }

  Span<const UtilityActorName> GetActors(
      const RefPtr<UtilityProcessParent>& aParent) {
    for (auto& p : mProcesses) {
      if (p->mProcessParent && p->mProcessParent == aParent) {
        return p->mActors;
      }
    }
    return {};
  }

  Span<const UtilityActorName> GetActors(GeckoChildProcessHost* aHost) {
    for (auto& p : mProcesses) {
      if (p->mProcess == aHost) {
        return p->mActors;
      }
    }
    return {};
  }

  Span<const UtilityActorName> GetActors(
      SandboxingKind aSbKind, const nsACString& aInstanceKey = ""_ns) {
    auto proc = GetProcess(aSbKind, aInstanceKey);
    if (!proc) {
      return {};
    }
    return proc->mActors;
  }

  // Shutdown the Utility process for that sandbox.
  void CleanShutdown(SandboxingKind aSandbox,
                     const nsACString& aInstanceKey = ""_ns);

  // Shutdown all utility processes
  void CleanShutdownAllProcesses();

  uint16_t AliveProcesses();

 private:
  ~UtilityProcessManager();

  bool IsProcessLaunching(SandboxingKind aSandbox,
                          const nsACString& aInstanceKey = ""_ns);
  bool IsProcessDestroyed(SandboxingKind aSandbox,
                          const nsACString& aInstanceKey = ""_ns);

  // Called from our xpcom-shutdown observer.
  void OnXPCOMShutdown();
  void OnPreferenceChange(const char16_t* aData);

  UtilityProcessManager();

  void Init();

  void DestroyProcess(SandboxingKind aSandbox,
                      const nsACString& aInstanceKey = ""_ns);

  bool IsShutdown() const;

  class Observer final : public nsIObserver {
   public:
    NS_DECL_ISUPPORTS
    NS_DECL_NSIOBSERVER
    explicit Observer(UtilityProcessManager* aManager);

   protected:
    ~Observer() = default;

    RefPtr<UtilityProcessManager> mManager;
  };
  friend class Observer;

  RefPtr<Observer> mObserver;

  class ProcessFields final {
   public:
    NS_INLINE_DECL_THREADSAFE_REFCOUNTING(ProcessFields);

    explicit ProcessFields(SandboxingKind aSandbox,
                           const nsACString& aInstanceKey = ""_ns)
        : mSandbox(aSandbox), mInstanceKey(aInstanceKey) {};

    // Promise will be resolved when this Utility process has been fully started
    // and configured. Only accessed on the main thread.
    RefPtr<SharedLaunchPromise<Ok>> mLaunchPromise;

    uint32_t mNumProcessAttempts = 0;
    uint32_t mNumUnexpectedCrashes = 0;

    // Fields that are associated with the current Utility process.
    UtilityProcessHost* mProcess = nullptr;
    RefPtr<UtilityProcessParent> mProcessParent = nullptr;

    // Collects any pref changes that occur during process launch (after
    // the initial map is passed in command-line arguments) to be sent
    // when the process can receive IPC messages.
    nsTArray<dom::Pref> mQueuedPrefs;

    nsTArray<UtilityActorName> mActors;

    SandboxingKind mSandbox = SandboxingKind::COUNT;

    // Distinguishes multiple concurrent processes of the same mSandbox kind
    // (see StartHWInference); always the empty string for kinds that only ever
    // have one.
    nsCString mInstanceKey;

    // The keep-alive handed out for this process, if any: it clears this on
    // destruction, see LaunchProcessWithKeepAlive.
    UtilityProcessKeepAlive* mKeepAlive = nullptr;

   protected:
    ~ProcessFields() = default;
  };

  // Holds every live process, keyed by (mSandbox, mInstanceKey): some kinds
  // (see StartHWInference) can have more than one live process at a time,
  // distinguished by mInstanceKey.
  nsTArray<RefPtr<ProcessFields>> mProcesses;

  RefPtr<ProcessFields> GetProcess(SandboxingKind,
                                   const nsACString& aInstanceKey = ""_ns);
  bool NoMoreProcesses();

  // Core of both StartUtility() entry points: binds aActor to aProcess once
  // aLaunchPromise resolves. The launch promise is passed in rather than read
  // off aProcess because LaunchProcess() can reject without storing one there.
  template <typename Actor>
  RefPtr<LaunchPromise<Ok>> StartUtilityOnProcess(
      RefPtr<Actor> aActor, ProcessFields* aProcess,
      RefPtr<SharedLaunchPromise<Ok>> aLaunchPromise);

#ifdef XP_WIN
  RefPtr<dom::WindowsUtilsParent> mWindowsUtils;
#endif  // XP_WIN
};

// Holds the utility process instance it was acquired on, and shuts that
// instance down once the last reference on it goes away. Handed out by
// UtilityProcessManager::LaunchProcessWithKeepAlive(). Main thread only.
class UtilityProcessKeepAlive final {
 public:
  NS_INLINE_DECL_REFCOUNTING(UtilityProcessKeepAlive);

  // Resolves once the instance this was acquired on has finished launching.
  RefPtr<UtilityProcessManager::SharedLaunchPromise<Ok>> GetLaunchPromise()
      const;

  // Like UtilityProcessManager::StartUtility(), but binds aActor to the process
  // this keep-alive was acquired on, rather than to whichever one is current
  // for its SandboxingKind.
  template <typename Actor>
  RefPtr<UtilityProcessManager::LaunchPromise<Ok>> StartUtility(
      RefPtr<Actor> aActor);

 private:
  friend class UtilityProcessManager;

  explicit UtilityProcessKeepAlive(
      UtilityProcessManager::ProcessFields* aProcess);
  ~UtilityProcessKeepAlive();

  const RefPtr<UtilityProcessManager::ProcessFields> mProcess;
};

}  // namespace ipc

}  // namespace mozilla

#endif  // _include_ipc_glue_UtilityProcessManager_h_
