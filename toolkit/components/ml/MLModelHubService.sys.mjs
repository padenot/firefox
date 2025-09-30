/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

/** @type {Lazy} */
const lazy = {};

ChromeUtils.defineLazyGetter(lazy, "console", () => {
  return console.createInstance({
    maxLogLevel: "Debug",
    prefix: "MLModelHubService",
  });
});

ChromeUtils.defineESModuleGetters(lazy, {
  ModelHub: "chrome://global/content/ml/ModelHub.sys.mjs",
  MLEngineParent: "resource://gre/actors/MLEngineParent.sys.mjs",
});

export function MLModelHubService() {
  this._modelHub = null;
  this._activeDownloads = new Map();
}

MLModelHubService.prototype = {
  classID: Components.ID("{e381b167-cb68-429c-9d57-35acf9054505}"),
  QueryInterface: ChromeUtils.generateQI(["nsIMLModelHub"]),

  async _getModelHub() {
    if (!this._modelHub) {
      lazy.console.debug("Creating model hub instance");
      const MODEL_HUB_ROOT_URL = Services.prefs.getStringPref(
        "browser.ml.modelHubRootUrl"
      );
      const MODEL_HUB_URL_TEMPLATE = Services.prefs.getStringPref(
        "browser.ml.modelHubUrlTemplate"
      );
      this._modelHub = new lazy.ModelHub({
        MODEL_HUB_ROOT_URL,
        MODEL_HUB_URL_TEMPLATE,
        allowDenyList: await lazy.MLEngineParent.getAllowDenyList(),
      });
    }

    return this._modelHub;
  },

  async isModelAvailable(aModel, aRevision, aFilename) {
    lazy.console.debug(
      `[XPCOM] isModelAvailable called for ${aModel}@${aRevision}`
    );

    if (!aModel || !aRevision) {
      lazy.console.warn("[XPCOM] ERROR: Model and revision must be provided");
      return false;
    }

    try {
      const modelHub = await this._getModelHub();

      lazy.console.info(
        `[XPCOM] Checking actual availability for ${aModel}/${aRevision}/filename via ModelHub`
      );

      const isAvailable = await modelHub.isModelAvailable(aModel, aRevision, { file: aFilename });

      lazy.console.info(
        `[XPCOM] Model ${aModel}@${aRevision} availability: ${isAvailable}`
      );

      return isAvailable;
    } catch (error) {
      lazy.console.error(
        `[XPCOM] ERROR: Exception checking model availability for ${aModel}@${aRevision}: ${error}`
      );
      return false;
    }
  },

  downloadModel(
    aTaskName,
    aModel,
    aRevision,
    aFiles,
    aProgressCallback,
    aCompletionCallback
  ) {
    lazy.console.info(
      `[XPCOM] downloadModel called for ${aModel}@${aRevision} with ${aFiles ? aFiles.length : 0} files`
    );

    if (!aTaskName || !aModel || !aRevision || !aFiles || aFiles.length === 0) {
      const error = "Task name, model, revision, and files must be provided";
      lazy.console.error(`[XPCOM] Download validation failed: ${error}`);
      if (aCompletionCallback) {
        try {
          aCompletionCallback.onError(error);
        } catch (e) {
          lazy.console.error("Error calling completion callback:", e);
        }
      }
      return "";
    }

    const sessionId = `${Date.now()}-${Math.random()}`;
    const downloadInfo = {
      sessionId,
      taskName: aTaskName,
      model: aModel,
      revision: aRevision,
      files: Array.from(aFiles),
      progressCallback: aProgressCallback,
      completionCallback: aCompletionCallback,
      downloadedFiles: new Map(),
      totalFiles: aFiles.length,
      completedFiles: 0,
    };

    this._activeDownloads.set(sessionId, downloadInfo);

    lazy.console.info(
      `[XPCOM] Created download session ${sessionId} for ${aModel}@${aRevision}, starting async download`
    );

    // Start the download process asynchronously
    this._startDownload(downloadInfo).catch(error => {
      lazy.console.error(`[XPCOM] Download failed for session ${sessionId}:`, error);
      this._completeDownload(sessionId, false, error.message);
    });

    return sessionId;
  },

  async _startDownload(downloadInfo) {
    const modelHub = await this._getModelHub();
    const {
      sessionId,
      taskName,
      model,
      revision,
      files,
      progressCallback,
    } = downloadInfo;

    lazy.console.info(
      `[XPCOM] Starting download session ${sessionId} for model ${model}@${revision} (${files.length} files)`
    );

    let totalLoaded = 0;
    let currentFileIndex = 0;

    for (const file of files) {
      try {
        lazy.console.info(
          `[XPCOM] Downloading file ${currentFileIndex + 1}/${files.length}: ${file} for model ${model}/${revision}`
        );

        const progressWrapper = progressCallback
          ? progressData => {
            const { progress, currentLoaded, total } = progressData;

            if (progressCallback) {
              try {
                // Calculate overall progress across all files
                const fileProgress = currentFileIndex / files.length * 100;
                const overallProgress = Math.floor(
                  fileProgress + (progress || 0) / files.length
                );

                lazy.console.info(
                  `[XPCOM] Download progress - File: ${file}, Progress: ${progress}%, Overall: ${overallProgress}%, Loaded: ${currentLoaded}/${total} bytes`
                );

                progressCallback.onProgress(
                  overallProgress,
                  currentLoaded || 0,
                  totalLoaded + (currentLoaded || 0),
                  total || 0
                );
              } catch (e) {
                lazy.console.error("Error calling progress callback:", e);
              }
            }
          }
          : null;

        const [localPath, headers] = await modelHub.getModelDataAsFile({
          engineId: "default-engine",
          taskName,
          model,
          revision,
          file,
          progressCallback: progressWrapper,
          featureId: "ml-model-hub-service",
          sessionId,
          telemetryData: { component: "MLModelHubService" },
        });

        downloadInfo.downloadedFiles.set(file, { localPath, headers });
        downloadInfo.completedFiles++;
        currentFileIndex++;

        if (headers && headers.fileSize) {
          totalLoaded += headers.fileSize;
        }

        lazy.console.info(
          `[XPCOM] Successfully downloaded file ${file} to ${localPath} (size: ${headers?.fileSize || 'unknown'} bytes)`
        );
      } catch (error) {
        lazy.console.error(`[XPCOM] Failed to download file ${file}:`, error);
        throw error;
      }
    }

    lazy.console.info(
      `[XPCOM] Download session ${sessionId} completed successfully - downloaded ${files.length} files, total size: ${totalLoaded} bytes`
    );
    this._completeDownload(sessionId, true);
  },

  _completeDownload(sessionId, success, errorMessage = null) {
    const downloadInfo = this._activeDownloads.get(sessionId);
    if (!downloadInfo) {
      lazy.console.warn(`[XPCOM] Download session ${sessionId} not found`);
      return;
    }

    const { completionCallback, model, revision } = downloadInfo;

    lazy.console.info(
      `[XPCOM] Completing download session ${sessionId} for ${model}@${revision} - success: ${success}`
    );

    if (completionCallback) {
      try {
        if (success) {
          lazy.console.info(
            `[XPCOM] Calling success callback for ${model}@${revision}`
          );
          completionCallback.onSuccess(model, revision);
        } else {
          lazy.console.warn(
            `[XPCOM] Calling error callback for ${model}@${revision}: ${errorMessage}`
          );
          completionCallback.onError(errorMessage || "Unknown error");
        }
      } catch (e) {
        lazy.console.error("Error calling completion callback:", e);
      }
    }

    this._activeDownloads.delete(sessionId);
    lazy.console.info(
      `[XPCOM] Download session ${sessionId} completed and cleaned up - success: ${success}`
    );
  },

  async getModelFilePath(aModel, aRevision, aFile) {
    if (!aModel || !aRevision || !aFile) {
      lazy.console.warn("Model, revision, and file must be provided");
      return "";
    }

    try {
      // getModelFilePath is deprecated - we should use getModelBlob instead
      // For now, just log and return empty string
      lazy.console.warn(
        `getModelFilePath called for ${aModel}/${aRevision}/${aFile} but should use getModelBlob for blob access`
      );
      return "";
    } catch (error) {
      lazy.console.error("Error getting model file path:", error);
      return "";
    }
  },

  async getModelBlob(aModel, aRevision, aFile) {
    lazy.console.info(
      `[XPCOM] getModelBlob called for ${aModel}/${aRevision}/${aFile}`
    );

    if (!aModel || !aRevision || !aFile) {
      lazy.console.error("[XPCOM] ERROR: Model, revision, and file must be provided");
      throw new Error("Model, revision, and file must be provided");
    }

    try {
      lazy.console.debug("[XPCOM] Step 1: Getting ModelHub instance");
      // ModelHub handles downloads transparently - use getModelFileAsBlob
      const modelHub = await this._getModelHub();
      lazy.console.debug("[XPCOM] Step 2: Got ModelHub, getting prefs");

      const MODEL_HUB_ROOT_URL = Services.prefs.getStringPref(
        "browser.ml.modelHubRootUrl"
      );
      const MODEL_HUB_URL_TEMPLATE = Services.prefs.getStringPref(
        "browser.ml.modelHubUrlTemplate"
      );

      lazy.console.debug(
        `[XPCOM] Step 3: Calling getModelFileAsBlob with params:\n` +
        `  engineId: speech-recognition\n` +
        `  taskName: speech-recognition\n` +
        `  model: ${aModel}\n` +
        `  revision: ${aRevision}\n` +
        `  file: ${aFile}\n` +
        `  modelHubRootUrl: ${MODEL_HUB_ROOT_URL}\n` +
        `  modelHubUrlTemplate: ${MODEL_HUB_URL_TEMPLATE}`
      );

      // Just call getModelFileAsBlob with all required parameters
      lazy.console.debug("[XPCOM] Calling getModelFileAsBlob");

      const result = await modelHub.getModelFileAsBlob({
        engineId: "speech-recognition",
        taskName: "speech-recognition",
        model: aModel,
        revision: aRevision,
        file: aFile,
        modelHubRootUrl: MODEL_HUB_ROOT_URL,
        modelHubUrlTemplate: MODEL_HUB_URL_TEMPLATE,
        progressCallback: null,
        featureId: "speech-recognition",
        sessionId: `sr-${Date.now()}`
      });

      lazy.console.debug("[XPCOM] getModelFileAsBlob completed");

      lazy.console.debug(`[XPCOM] Step 4: File retrieved, checking result`);

      if (!result || !Array.isArray(result) || result.length < 1) {
        lazy.console.error(`[XPCOM] ERROR: Invalid result from getModelFileAsBlob:`, result);
        throw new Error("Invalid result from getModelFileAsBlob");
      }

      const [blob, headers] = result;

      if (!blob) {
        lazy.console.error("[XPCOM] ERROR: No blob returned from getModelFileAsBlob");
        throw new Error("No blob returned");
      }

      lazy.console.info(
        `[XPCOM] Step 5: SUCCESS - Got blob for ${aModel}/${aRevision}/${aFile}, size: ${blob.size} bytes`
      );

      return blob;
    } catch (error) {
      lazy.console.error(`[XPCOM] ERROR in getModelBlob:`, error);
      lazy.console.error(`[XPCOM] Error stack:`, error.stack);
      throw error;
    }
  },
};
