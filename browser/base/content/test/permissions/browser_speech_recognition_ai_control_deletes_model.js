/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Blocking on-device speech recognition through AI Controls must delete its
// downloaded model, however the state is flipped (UI toggle, policy,
// about:config, sync). SpeechRecognitionModelCleanup observes the control prefs
// in the parent process and evicts the "parakeet-gguf" engine's files.

const { sinon } = ChromeUtils.importESModule(
  "resource://testing-common/Sinon.sys.mjs"
);
const { MLUninstallService } = ChromeUtils.importESModule(
  "chrome://global/content/ml/Utils.sys.mjs"
);

const ENGINE_ID = "parakeet-gguf";

registerCleanupFunction(() => {
  Services.prefs.clearUserPref("browser.ai.control.speechRecognition");
  Services.prefs.clearUserPref("browser.ai.control.default");
});

add_task(async function test_blocking_feature_pref_deletes_model() {
  let stub = sinon.stub(MLUninstallService, "uninstall").resolves();
  try {
    await SpecialPowers.pushPrefEnv({
      set: [["browser.ai.control.speechRecognition", "blocked"]],
    });
    await TestUtils.waitForCondition(
      () => stub.called,
      "uninstall called when speechRecognition pref is blocked"
    );
    ok(
      stub.calledWithMatch({ engineIds: [ENGINE_ID] }),
      "deletes the parakeet-gguf engine's model"
    );
    await SpecialPowers.popPrefEnv();
  } finally {
    stub.restore();
  }
});

add_task(async function test_blocking_via_global_default_deletes_model() {
  let stub = sinon.stub(MLUninstallService, "uninstall").resolves();
  try {
    await SpecialPowers.pushPrefEnv({
      set: [
        ["browser.ai.control.speechRecognition", "default"],
        ["browser.ai.control.default", "blocked"],
      ],
    });
    await TestUtils.waitForCondition(
      () => stub.called,
      "uninstall called when the global default falls back to blocked"
    );
    ok(
      stub.calledWithMatch({ engineIds: [ENGINE_ID] }),
      "deletes the parakeet-gguf engine's model via the global default"
    );
    await SpecialPowers.popPrefEnv();
  } finally {
    stub.restore();
  }
});

add_task(async function test_making_available_does_not_delete_model() {
  let stub = sinon.stub(MLUninstallService, "uninstall").resolves();
  try {
    await SpecialPowers.pushPrefEnv({
      set: [["browser.ai.control.speechRecognition", "available"]],
    });
    await TestUtils.waitForTick();
    ok(!stub.called, "does not delete the model when set to available");
    await SpecialPowers.popPrefEnv();
  } finally {
    stub.restore();
  }
});
