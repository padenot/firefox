/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ml_MLModelHubChild_h
#define mozilla_ml_MLModelHubChild_h

#include "mozilla/ml/PMLModelHubChild.h"
#include "mozilla/MozPromise.h"
#include "mozilla/ipc/Endpoint.h"
#include "mozilla/dom/IPCBlob.h"

namespace mozilla::ml {

using ModelAvailabilityPromise = MozPromise<bool, nsCString, false>;
using ModelDownloadPromise =
    MozPromise<bool, nsCString, false>;
using ModelBlobPromise = MozPromise<mozilla::dom::IPCBlob, nsCString, false>;

// Progress callback interface
class ModelDownloadProgressCallback {
 public:
  virtual ~ModelDownloadProgressCallback() = default;
  virtual void OnProgress(int32_t aProgress, int64_t aCurrentLoaded,
                          int64_t aTotalLoaded, int64_t aTotal) = 0;
};

// HWUtility-side IPDL actor for ModelHub operations.
// This actor runs in the HWInference utility process and makes calls to the
// main process.
class MLModelHubChild final : public PMLModelHubChild {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MLModelHubChild, override);

  MLModelHubChild() = default;
  static already_AddRefed<MLModelHubChild> Create();

  RefPtr<ModelAvailabilityPromise> IsModelAvailable(const nsCString& aModel,
                                                    const nsCString& aRevision,
                                                    const nsCString& aFilename);

  RefPtr<ModelDownloadPromise> DownloadModel(
      const nsCString& aTaskName, const nsCString& aModel,
      const nsCString& aRevision, const nsTArray<nsCString>& aFiles,
      ModelDownloadProgressCallback* aProgressCallback = nullptr);

  RefPtr<ModelBlobPromise> GetModelBlob(const nsCString& aModel,
                                        const nsCString& aRevision,
                                        const nsCString& aFile);

  // PMLModelHubChild implementation
  mozilla::ipc::IPCResult RecvOnModelDownloadProgress(
      nsCString&& aSessionId, ModelDownloadProgress&& aProgress);

  mozilla::ipc::IPCResult RecvOnModelDownloadComplete(
      nsCString&& aSessionId, ModelDownloadResult&& aResult);

  void ActorDestroy(ActorDestroyReason aReason) override;

 private:
  ~MLModelHubChild() = default;

  // Structure to track ongoing download requests
  struct PendingDownload {
    RefPtr<ModelDownloadPromise::Private> mPromise;
    ModelDownloadProgressCallback* mProgressCallback;

    PendingDownload(ModelDownloadPromise::Private* aPromise,
                    ModelDownloadProgressCallback* aProgressCallback)
        : mPromise(aPromise), mProgressCallback(aProgressCallback) {}
  };

  // Map of session IDs to pending download requests
  nsTHashMap<nsCStringHashKey, UniquePtr<PendingDownload>> mPendingDownloads;
};

// Singleton service for to use ModelHub from the utility process
class MLModelHubService final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(MLModelHubService);

  static MLModelHubService& GetSingleton();

  // Initialize the service with an IPDL endpoint
  bool Initialize(ipc::Endpoint<PMLModelHubChild>&& aEndpoint);

  // Public API for utility process callers
  RefPtr<ModelAvailabilityPromise> IsModelAvailable(const nsCString& aModel,
                                                    const nsCString& aRevision,
                                                    const nsCString& aFilename);

  RefPtr<ModelDownloadPromise> DownloadModel(
      const nsCString& aTaskName, const nsCString& aModel,
      const nsCString& aRevision, const nsTArray<nsCString>& aFiles,
      ModelDownloadProgressCallback* aProgressCallback = nullptr);

  RefPtr<ModelBlobPromise> GetModelBlob(const nsCString& aModel,
                                        const nsCString& aRevision,
                                        const nsCString& aFile);

  // Called during process shutdown
  void Shutdown();

 private:
  ~MLModelHubService() = default;

  RefPtr<MLModelHubChild> mChild;
  mozilla::Mutex mMutex{"MLModelHubService"};
};

} // namespace mozilla::ml


#endif  // mozilla_ml_MLModelHubChild_h
