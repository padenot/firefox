/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// When on-device speech recognition is disabled in AI Controls, a page's
// attempt to use it (install() or start()) surfaces a notification bar offering
// to turn the feature on inline, or to open the AI Controls settings. The gate
// is read live, so re-enabling the setting lets a subsequent call proceed.

const ORIGIN = "https://example.com";
const PAGE =
  getRootDirectory(gTestPath).replace("chrome://mochitests/content", ORIGIN) +
  "empty.html";

const NOTIFICATION_VALUE = "speech-recognition-ai-blocked";

add_setup(async function () {
  await SpecialPowers.pushPrefEnv({
    set: [["media.webspeech.recognition.enable", true]],
  });
});

function waitForBlockedInfobar(browser) {
  let box = gBrowser.getNotificationBox(browser);
  return TestUtils.waitForCondition(
    () => box.getNotificationWithValue(NOTIFICATION_VALUE),
    "blocked notification bar shown"
  );
}

async function blocked(callback) {
  await SpecialPowers.pushPrefEnv({
    set: [["browser.ai.control.speechRecognition", "blocked"]],
  });
  await BrowserTestUtils.withNewTab(PAGE, callback);
  await SpecialPowers.popPrefEnv();
}

add_task(async function test_install_when_blocked_shows_infobar() {
  await blocked(async browser => {
    await SpecialPowers.spawn(browser, [], async () => {
      content.document.notifyUserGestureActivation();
      // install() rejects with NotAllowedError while blocked; the notification
      // bar is shown regardless.
      await content.SpeechRecognition.install({
        langs: ["en-US"],
        processLocally: true,
      }).then(
        () => Assert.ok(false, "install() should reject while blocked"),
        err => Assert.equal(err.name, "NotAllowedError", "install() rejects")
      );
    });
    let notification = await waitForBlockedInfobar(browser);
    ok(notification, "blocked notification bar shown for install()");
  });
});

add_task(async function test_start_when_blocked_shows_infobar() {
  await blocked(async browser => {
    await SpecialPowers.spawn(browser, [], () => {
      let recognition = new content.SpeechRecognition();
      Assert.throws(
        () => recognition.start(),
        /NotAllowedError/,
        "start() throws NotAllowedError while blocked"
      );
    });
    let notification = await waitForBlockedInfobar(browser);
    ok(notification, "blocked notification bar shown for start()");
  });
});

// Opting in from the notification bar turns on just this feature, leaving any
// other AI Controls state alone.
add_task(async function test_enable_opts_in_to_speech_recognition() {
  await blocked(async browser => {
    await SpecialPowers.spawn(browser, [], () => {
      content.document.notifyUserGestureActivation();
      content.SpeechRecognition.install({
        langs: ["en-US"],
        processLocally: true,
      }).catch(() => {});
    });
    let notification = await waitForBlockedInfobar(browser);

    notification.buttonContainer.querySelectorAll("button")[0].click();

    Assert.equal(
      Services.prefs.getCharPref("browser.ai.control.speechRecognition"),
      "enabled",
      "Enable opted in to speech recognition"
    );
  });
});

add_task(async function test_view_ai_controls_deep_links_to_preferences() {
  await blocked(async browser => {
    await SpecialPowers.spawn(browser, [], () => {
      content.document.notifyUserGestureActivation();
      content.SpeechRecognition.install({
        langs: ["en-US"],
        processLocally: true,
      }).catch(() => {});
    });
    let notification = await waitForBlockedInfobar(browser);

    let prefTabPromise = BrowserTestUtils.waitForNewTab(
      gBrowser,
      url => url.startsWith("about:preferences"),
      true
    );

    notification.buttonContainer.querySelectorAll("button")[1].click();

    let prefTab = await prefTabPromise;
    // The URL fragment canonicalizes to the pane ("#ai"); the
    // "speechRecognition" subcategory is applied as an in-page spotlight.
    let spec = prefTab.linkedBrowser.currentURI.spec;
    ok(
      spec.startsWith("about:preferences#ai"),
      `View AI Controls deep-links to the AI Controls pane (got ${spec})`
    );
    let control = prefTab.linkedBrowser.contentDocument.querySelector(
      '[data-subcategory~="speechRecognition"]'
    );
    ok(control, "the speech recognition setting has a spotlight anchor");
    BrowserTestUtils.removeTab(prefTab);
  });
});

// The gate is read live on each call, so flipping the setting back to available
// lets a subsequent install() proceed without the AI-blocked error.
add_task(async function test_reenabling_lets_install_proceed() {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["browser.ml.modelHub.testing", true],
      ["media.webspeech.recognition.model-download.prompt.testing", true],
      ["media.navigator.permission.disabled", true],
    ],
  });

  await BrowserTestUtils.withNewTab(PAGE, async browser => {
    await SpecialPowers.pushPrefEnv({
      set: [["browser.ai.control.speechRecognition", "blocked"]],
    });
    await SpecialPowers.spawn(browser, [], async () => {
      content.document.notifyUserGestureActivation();
      await content.SpeechRecognition.install({
        langs: ["en-US"],
        processLocally: true,
      }).then(
        () => Assert.ok(false, "install() should reject while blocked"),
        err =>
          Assert.equal(
            err.name,
            "NotAllowedError",
            "install() rejects while blocked"
          )
      );
    });
    await SpecialPowers.popPrefEnv();

    await SpecialPowers.pushPrefEnv({
      set: [["browser.ai.control.speechRecognition", "available"]],
    });
    let installed = await SpecialPowers.spawn(browser, [], () => {
      content.document.notifyUserGestureActivation();
      return content.SpeechRecognition.install({
        langs: ["en-US"],
        processLocally: true,
      });
    });
    ok(installed, "install() proceeds once the feature is re-enabled");
    await SpecialPowers.popPrefEnv();
  });

  await SpecialPowers.popPrefEnv();
});
