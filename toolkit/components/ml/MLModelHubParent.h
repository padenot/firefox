/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ml_MLModelHubParent_h
#define mozilla_ml_MLModelHubParent_h

#include "mozilla/ml/PMLModelHubParent.h"
#include "nsIMLModelHub.h"
#include "nsCOMPtr.h"
#include "nsTHashMap.h"
#include "nsString.h"
#include "nsRefPtrHashtable.h"

namespace mozilla {
namespace ml {

// Forward declarations
class MLModelHubManager;

// Progress callback implementation for XPCOM ModelHub service
class ModelHubProgressCallback final : public nsIMLModelDownloadProgressCallback {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIMLMODELDOWNLOADPROGRESSCALLBACK

  explicit ModelHubProgressCallback(PMLModelHubParent* aParent,
                                   const nsCString& aSessionId);

  RefPtr<PMLModelHubParent> mParent;
  nsCString mSessionId;

 private:
  ~ModelHubProgressCallback() = default;
};

// Completion callback implementation for XPCOM ModelHub service
class ModelHubCompletionCallback final : public nsIMLModelDownloadCompletionCallback {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIMLMODELDOWNLOADCOMPLETIONCALLBACK

  explicit ModelHubCompletionCallback(PMLModelHubParent* aParent,
                                     const nsCString& aSessionId);

  RefPtr<PMLModelHubParent> mParent;
  nsCString mSessionId;

 private:
  ~ModelHubCompletionCallback() = default;
};

// Main process side IPDL actor for ModelHub operations
// This actor runs in the main process and handles requests from utility processes
class MLModelHubParent final : public PMLModelHubParent {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MLModelHubParent, override);

  static already_AddRefed<MLModelHubParent> Create();

  // PMLModelHubParent implementation
  mozilla::ipc::IPCResult RecvIsModelAvailable(
      nsCString&& aModel, nsCString&& aRevision, nsCString&& aFilename,
      IsModelAvailableResolver&& aResolver);

  mozilla::ipc::IPCResult RecvStartModelDownload(
      nsCString&& aTaskName, nsCString&& aModel, nsCString&& aRevision,
      nsTArray<nsCString>&& aFiles,
      StartModelDownloadResolver&& aResolver);

  mozilla::ipc::IPCResult RecvGetModelBlob(
      nsCString&& aModel, nsCString&& aRevision, nsCString&& aFile,
      GetModelBlobResolver&& aResolver);

  void ActorDestroy(ActorDestroyReason aReason) override;

  // Called by callbacks to notify the utility process
  void NotifyProgress(const nsCString& aSessionId, int32_t aProgress,
                      int64_t aCurrentLoaded, int64_t aTotalLoaded,
                      int64_t aTotal);
  void NotifyComplete(const nsCString& aSessionId, const nsCString& aModel,
                      const nsCString& aRevision);
  void NotifyError(const nsCString& aSessionId, const nsCString& aError);

 private:
  MLModelHubParent() = default;
  ~MLModelHubParent() = default;

  // Reference to the XPCOM ModelHub service
  nsCOMPtr<nsIMLModelHub> mModelHubService;

  // Map to track active download sessions and their callbacks
  nsTHashMap<nsCStringHashKey, RefPtr<ModelHubProgressCallback>> mProgressCallbacks;
  nsTHashMap<nsCStringHashKey, RefPtr<ModelHubCompletionCallback>> mCompletionCallbacks;
};

// Singleton manager for MLModelHub operations in the parent process
class MLModelHubManager final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MLModelHubManager);

  static MLModelHubManager& GetSingleton();

  // Get or create the ModelHub parent actor
  already_AddRefed<MLModelHubParent> GetOrCreateParent();

  // Called during process shutdown
  void Shutdown();

 private:
  MLModelHubManager() = default;
  ~MLModelHubManager() = default;

  RefPtr<MLModelHubParent> mParent;
  mozilla::Mutex mMutex{"MLModelHubManager"};
};

} // namespace ml
} // namespace mozilla

#endif // mozilla_ml_MLModelHubParent_h
