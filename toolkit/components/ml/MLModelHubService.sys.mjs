/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { XPCOMUtils } from "resource://gre/modules/XPCOMUtils.sys.mjs";

const lazy = XPCOMUtils.declareLazy({
  ModelHub: "chrome://global/content/ml/ModelHub.sys.mjs",
  MLEngineParent: "resource://gre/actors/MLEngineParent.sys.mjs",
  console: () => {
    return console.createInstance({
      maxLogLevel: "Debug",
      prefix: "MLModelHubService",
    });
  },
});

// Thin XPCOM layer around ModelHub.sys.mjs, to be able to interact with it from
// native code.
export class MLModelHubService {
  constructor() {
    this._modelHub = null;
    this._activeDownloads = new Map();
  }

  classID = Components.ID("{e381b167-cb68-429c-9d57-35acf9054505}");
  QueryInterface = ChromeUtils.generateQI(["nsIMLModelHub"]);

  async _getModelHub() {
    if (!this._modelHub) {
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
  }

  async isModelAvailable(aModel, aRevision, aFilename) {
    if (!aModel || !aRevision || !aFilename) {
      lazy.console.error(
        `isModelAvailable, invalid arguments: {aModel: ${aModel},
         aRevision: ${aRevision}, aFilename: ${aFilename}}`
      );
      return false;
    }

    try {
      const modelHub = await this._getModelHub();

      lazy.console.info(
        `[XPCOM] Checking actual availability for ${aModel}/${aRevision}/${aFilename}`
      );

      const isAvailable = await modelHub.isModelAvailable(aModel, aRevision, {
        file: aFilename,
      });

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
  }

  downloadModel(
    aTaskName,
    aModel,
    aRevision,
    aFiles,
    aProgressCallback,
    aCompletionCallback
  ) {
    lazy.console.info(
      `MLModelHubService.downloadModel called for ${aModel}@${aRevision} with ${aFiles ? aFiles.length : 0} files`
    );

    if (!aTaskName || !aModel || !aRevision || !aFiles || aFiles.length === 0) {
      const error = "Task name, model, revision, and files must be provided";
      lazy.console.error(`Download argument validation failed: ${error}`);
      if (aCompletionCallback) {
        try {
          aCompletionCallback.onError(error);
        } catch (e) {
          lazy.console.error("Error calling completion callback:", e);
        }
      }
      return error;
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
      `Created download session ${sessionId} for ${aModel}@${aRevision}, starting async download`
    );

    this._startDownload(downloadInfo).catch(error => {
      lazy.console.error(`Download failed for session ${sessionId}:`, error);
      this._completeDownload(sessionId, false, error.message);
    });

    return sessionId;
  }

  async _startDownload(downloadInfo) {
    const modelHub = await this._getModelHub();
    const { sessionId, taskName, model, revision, files, progressCallback } =
      downloadInfo;

    lazy.console.info(
      `Starting download session ${sessionId} for model ${model}@${revision} (${files.length} files)`
    );

    let totalLoaded = 0;
    let currentFileIndex = 0;

    for (const file of files) {
      try {
        lazy.console.info(
          `Downloading file ${currentFileIndex + 1}/${files.length}: ${file} for model ${model}/${revision}`
        );

        const progressWrapper = progressCallback
          ? progressData => {
              const { progress, currentLoaded, total } = progressData;

              if (progressCallback) {
                try {
                  // Calculate overall progress across all files
                  const fileProgress = (currentFileIndex / files.length) * 100;
                  const overallProgress = Math.floor(
                    fileProgress + (progress || 0) / files.length
                  );

                  lazy.console.debug(
                    `Download progress - File: ${file}, Progress: ${progress}%,
                     Overall: ${overallProgress}%, Loaded: ${currentLoaded}/${total} bytes`
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
          engineId: taskName,
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
          `Successfully downloaded file ${file} to ${localPath} (size: ${headers?.fileSize || "unknown"} bytes)`
        );
      } catch (error) {
        lazy.console.error(`Failed to download file ${file}:`, error);
        throw error;
      }
    }

    lazy.console.info(
      `Download session ${sessionId} completed successfully - downloaded
       ${files.length} files, total size: ${totalLoaded} bytes`
    );
    this._completeDownload(sessionId, true);
  }

  _completeDownload(sessionId, success, errorMessage = null) {
    const downloadInfo = this._activeDownloads.get(sessionId);
    if (!downloadInfo) {
      lazy.console.error(`Download session ${sessionId} not found`);
      return;
    }

    const { completionCallback, model, revision } = downloadInfo;

    lazy.console.info(
      `Completing download session ${sessionId} for ${model}@${revision} - success: ${success}`
    );

    if (completionCallback) {
      try {
        if (success) {
          completionCallback.onSuccess(model, revision);
        } else {
          completionCallback.onError(errorMessage || "Unknown error");
        }
      } catch (e) {
        lazy.console.error("Error calling completion callback:", e);
      }
    }

    this._activeDownloads.delete(sessionId);
  }

  async getModelBlob(aEngineId, aTaskName, aModel, aRevision, aFile) {
    lazy.console.info(
      `getModelBlob called for ${aModel}/${aRevision}/${aFile} with engine ${aEngineId} and task ${aTaskName}`
    );

    if (!aEngineId || !aTaskName || !aModel || !aRevision || !aFile) {
      lazy.console.error(
        "ERROR: Engine ID, task name, model, revision, and file must be provided"
      );
      throw new Error(
        "Engine ID, task name, model, revision, and file must be provided"
      );
    }

    try {
      const modelHub = await this._getModelHub();
      const MODEL_HUB_ROOT_URL = Services.prefs.getStringPref(
        "browser.ml.modelHubRootUrl"
      );
      const MODEL_HUB_URL_TEMPLATE = Services.prefs.getStringPref(
        "browser.ml.modelHubUrlTemplate"
      );

      const result = await modelHub.getModelFileAsBlob({
        engineId: aEngineId,
        taskName: aTaskName,
        model: aModel,
        revision: aRevision,
        file: aFile,
        modelHubRootUrl: MODEL_HUB_ROOT_URL,
        modelHubUrlTemplate: MODEL_HUB_URL_TEMPLATE,
        progressCallback: null,
        featureId: aTaskName,
        sessionId: `${aTaskName}-${Date.now()}`,
      });

      if (!result || !Array.isArray(result) || result.length < 1) {
        lazy.console.error(
          `ERROR: Invalid result from getModelFileAsBlob:`,
          result
        );
        throw new Error("Invalid result from getModelFileAsBlob");
      }

      const [blob] = result;

      if (!blob) {
        lazy.console.error("ERROR: No blob returned from getModelFileAsBlob");
        throw new Error("No blob returned");
      }

      return blob;
    } catch (error) {
      lazy.console.error(`[XPCOM] ERROR in getModelBlob:`, error, error.stack);
      throw error;
    }
  }
}
