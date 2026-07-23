/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Parent-process component that deletes the on-device speech recognition model
// when the feature becomes blocked via AI Controls. Blocking gates recognition
// on its own (the SpeechRecognition DOM API reads the prefs live), but that
// leaves the downloaded model on disk; other AI-Controls features delete their
// models on block, so speech recognition does too. Observing the prefs here
// (rather than only in the about:preferences toggle) means the model is deleted
// however the state was flipped: the UI, an enterprise policy, about:config, or
// sync.

const lazy = {};

ChromeUtils.defineESModuleGetters(lazy, {
  MLUninstallService: "chrome://global/content/ml/Utils.sys.mjs",
});

// The single engine id under which every speech recognition model artifact is
// stored (kSpeechRecognitionEngineId in SpeechRecognitionModelMapping.h);
// deleting by engine removes whatever language/quant subset was downloaded.
const SPEECH_RECOGNITION_ENGINE_ID = "parakeet-gguf";

const FEATURE_PREF = "browser.ai.control.speechRecognition";
const DEFAULT_PREF = "browser.ai.control.default";

// Mirror of SpeechRecognition.cpp's IsBlockedByAIControls: the feature pref
// wins unless unset/"default", in which case the global default applies.
function isBlockedByAIControls() {
  let state = Services.prefs.getStringPref(FEATURE_PREF, "");
  if (!state || state == "default") {
    state = Services.prefs.getStringPref(DEFAULT_PREF, "");
  }
  return state == "blocked";
}

// Fire-and-forget: deleting an absent model is a no-op, so this is safe to call
// on every transition into the blocked state.
function deleteSpeechModel() {
  lazy.MLUninstallService.uninstall({
    engineIds: [SPEECH_RECOGNITION_ENGINE_ID],
    actor: "SpeechRecognition",
  }).catch(error => {
    console.error(
      "Failed to delete speech recognition model on AI Controls block:",
      error
    );
  });
}

export function SpeechRecognitionModelCleanup() {}

SpeechRecognitionModelCleanup.prototype = {
  classID: Components.ID("{efde81cd-0575-4a71-8fd9-6e4d107fb5a6}"),
  contractID: "@mozilla.org/dom/speech-recognition-model-cleanup;1",

  QueryInterface: ChromeUtils.generateQI(["nsIObserver"]),

  observe(subject, topic) {
    switch (topic) {
      case "profile-after-change":
        Services.prefs.addObserver(FEATURE_PREF, this);
        Services.prefs.addObserver(DEFAULT_PREF, this);
        break;
      case "nsPref:changed":
        if (isBlockedByAIControls()) {
          deleteSpeechModel();
        }
        break;
    }
  },
};
