/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

/// <reference path="head.js" />

/**
 * Test progress callback implementation
 */
function TestProgressCallback() {}
TestProgressCallback.prototype = {
  QueryInterface: ChromeUtils.generateQI(["nsIMLModelDownloadProgressCallback"]),

  onProgress(aProgress, aCurrentLoaded, aTotalLoaded, aTotal) {
    info(`Progress: ${aProgress}% (${aCurrentLoaded}/${aTotalLoaded} bytes, total: ${aTotal})`);
  }
};

/**
 * Test completion callback implementation with Promise support
 */
function TestCompletionCallback() {
  this.promise = new Promise((resolve, reject) => {
    this._resolve = resolve;
    this._reject = reject;
  });
}
TestCompletionCallback.prototype = {
  QueryInterface: ChromeUtils.generateQI(["nsIMLModelDownloadCompletionCallback"]),

  onSuccess(aModel, aRevision) {
    info(`Download completed successfully: ${aModel} v${aRevision}`);
    this._resolve({ success: true, model: aModel, revision: aRevision });
  },

  onError(aError) {
    info(`Download failed: ${aError}`);
    this._resolve({ success: false, error: aError });
  }
};

/**
 * Tests basic XPCOM service instantiation and model availability checking
 */
add_task(async function test_modelhub_service_basic() {
  info("Testing MLModelHub XPCOM component basic functionality");

  // Get the service
  const modelHub = Cc["@mozilla.org/ml-modelhub;1"].getService(Ci.nsIMLModelHub);
  Assert.ok(modelHub, "Successfully got MLModelHub service");

  // Test model availability check with valid input (now async)
  const isAvailable1 = await modelHub.isModelAvailable("mozilla/distilbert-base", "v1.0");
  Assert.equal(typeof isAvailable1, "boolean", "isModelAvailable returns a boolean");
  info(`mozilla/distilbert-base v1.0 availability: ${isAvailable1}`);

  // Test model availability check with empty model name (should return false)
  const isAvailable2 = await modelHub.isModelAvailable("", "v1.0");
  Assert.equal(isAvailable2, false, "Empty model name should return false");

  // Test model availability check with invalid model name (should now return false)
  const isAvailable3 = await modelHub.isModelAvailable("invalid-model-name", "v1.0");
  Assert.equal(typeof isAvailable3, "boolean", "Invalid model name still returns boolean");
  Assert.equal(isAvailable3, false, "Unknown model should return false");
  info(`invalid-model-name v1.0 availability: ${isAvailable3}`);
});

/**
 * Tests model availability heuristics
 */
add_task(async function test_modelhub_availability_heuristics() {
  info("Testing MLModelHub availability heuristics");

  const modelHub = Cc["@mozilla.org/ml-modelhub;1"].getService(Ci.nsIMLModelHub);

  // Test models that should be available (real availability check)
  const available1 = await modelHub.isModelAvailable("mozilla/distilbert-base", "v1.0");
  Assert.equal(available1, true, "Test models should be available");

  // Test unknown models (should now do real checks)
  const available2 = await modelHub.isModelAvailable("unknown/random-model", "v1.0");
  Assert.equal(available2, false, "Unknown models should not be available");

  // Test invalid model format
  const available3 = await modelHub.isModelAvailable("invalid-format", "v1.0");
  Assert.equal(available3, false, "Invalid format models should not be available");
});

/**
 * Tests the getModelFilePath method
 */
add_task(async function test_modelhub_get_file_path() {
  info("Testing MLModelHub getModelFilePath method");

  const modelHub = Cc["@mozilla.org/ml-modelhub;1"].getService(Ci.nsIMLModelHub);

  // Test file path getter (currently returns empty string as per implementation)
  const filePath = modelHub.getModelFilePath("mozilla/distilbert-base", "v1.0", "model.onnx");
  Assert.equal(typeof filePath, "string", "getModelFilePath returns a string");
  info(`File path result: "${filePath}"`);

  // Test with empty parameters
  const emptyPath = modelHub.getModelFilePath("", "", "");
  Assert.equal(emptyPath, "", "Empty parameters should return empty string");
});

/**
 * Tests the downloadModel method with valid parameters and waits for completion
 */
add_task(async function test_modelhub_download_valid() {
  info("Testing MLModelHub downloadModel with valid parameters");

  const modelHub = Cc["@mozilla.org/ml-modelhub;1"].getService(Ci.nsIMLModelHub);

  // Create callbacks
  const progressCallback = new TestProgressCallback();
  const completionCallback = new TestCompletionCallback();

  // Test download with valid parameters
  const sessionId = modelHub.downloadModel(
    "speech-recognition",
    "mozilla/distilbert-base",
    "v1.0",
    ["model.onnx", "tokenizer.json"],
    progressCallback,
    completionCallback
  );

  Assert.equal(typeof sessionId, "string", "downloadModel returns a string session ID");
  Assert.notEqual(sessionId, "", "Session ID should not be empty for valid parameters");
  info(`Download initiated with session ID: ${sessionId}`);

  // Wait for the download to complete
  info("Waiting for download to complete...");
  const result = await completionCallback.promise;

  // Verify the download succeeded
  Assert.equal(result.success, true, "Download should succeed with valid test files");
  if (result.success) {
    Assert.equal(result.model, "mozilla/distilbert-base", "Downloaded model name should match");
    Assert.equal(result.revision, "v1.0", "Downloaded revision should match");
  } else {
    Assert.ok(false, `Download failed with error: ${result.error}`);
  }
});

/**
 * Tests the downloadModel method with invalid parameters
 */
add_task(async function test_modelhub_download_invalid() {
  info("Testing MLModelHub downloadModel with invalid parameters");

  const modelHub = Cc["@mozilla.org/ml-modelhub;1"].getService(Ci.nsIMLModelHub);
  const completionCallback = new TestCompletionCallback();

  // Test invalid download (empty task name should fail)
  const sessionId2 = modelHub.downloadModel(
    "", // Empty task name
    "mozilla/distilbert-base",
    "v1.0",
    ["model.onnx"],
    null,
    completionCallback
  );

  Assert.equal(sessionId2, "", "Invalid download should return empty session ID");
  info(`Invalid download test result: "${sessionId2}" (correctly empty)`);

  // Wait for the error callback to be called
  const result = await completionCallback.promise;
  Assert.equal(result.success, false, "Download should fail with invalid parameters");
  Assert.ok(result.error.includes("Task name"), "Error should mention missing task name");
});

/**
 * Tests error handling with null parameters
 */
add_task(async function test_modelhub_null_parameters() {
  info("Testing MLModelHub with null/undefined parameters");

  const modelHub = Cc["@mozilla.org/ml-modelhub;1"].getService(Ci.nsIMLModelHub);

  // Test model availability with null revision
  try {
    const result = modelHub.isModelAvailable("test-model", null);
    Assert.equal(result, false, "Null revision should return false");
  } catch (e) {
    info("Expected behavior: null parameters might throw or return false");
  }

  // Test file path with undefined file parameter
  try {
    const filePath = modelHub.getModelFilePath("test", "v1", undefined);
    Assert.equal(typeof filePath, "string", "Should handle undefined gracefully");
  } catch (e) {
    info("Expected behavior: undefined parameters might throw");
  }
});