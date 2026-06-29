/* Any copyright is dedicated to the Public Domain.
   https://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

requestLongerTimeout(3);

add_setup(async function setupPrefs() {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["browser.preferences.aiControls", true],
      ["browser.ai.control.default", "available"],
      ["browser.ai.control.speechRecognition", "default"],
    ],
  });
});

describe("settings ai features", () => {
  describe("speech recognition AI control", () => {
    it("is registered and rendered, bound to its pref", async () => {
      await withPrefsPane("ai", async doc => {
        const win = doc.documentGlobal;

        // The backing Preference must be registered (Preferences.addAll); a
        // missing registration throws PreferenceNotAddedError when the control
        // renders.
        const pref = win.Preferences.get(
          "browser.ai.control.speechRecognition"
        );
        Assert.ok(
          pref,
          "browser.ai.control.speechRecognition Preference is registered"
        );

        const setting = win.Preferences.getSetting(
          "aiControlSpeechRecognitionSelect"
        );
        Assert.ok(setting, "speech recognition AI control setting exists");

        const control = doc.getElementById("aiControlSpeechRecognitionSelect");
        Assert.ok(control, "speech recognition AI control element exists");
        Assert.ok(
          BrowserTestUtils.isVisible(control),
          "speech recognition AI control is visible"
        );

        // The control reflects the backing pref.
        await SpecialPowers.pushPrefEnv({
          set: [["browser.ai.control.speechRecognition", "blocked"]],
        });
        setting.emit("change");
        await new Promise(r => win.requestAnimationFrame(r));
        Assert.equal(
          pref.value,
          "blocked",
          "the setting's Preference tracks browser.ai.control.speechRecognition"
        );
      });
    });
  });
});
