/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Verify that SpeechRecognition.install() shows a permission doorhanger and
// that accepting it resolves the promise with true, while dismissing resolves
// with false. Also verifies the prompt is skipped entirely once the model is
// already installed.

const ORIGIN = "https://example.com";
const PAGE =
  getRootDirectory(gTestPath).replace("chrome://mochitests/content", ORIGIN) +
  "empty.html";

async function triggerInstall(browser) {
  await SpecialPowers.spawn(browser, [], () => {
    content.document.notifyUserGestureActivation();
    // Do not await — we need to return immediately so the popup can appear.
    content.SpeechRecognition.install({
      langs: ["en-US"],
      processLocally: true,
    });
  });
}

add_task(async function test_install_shows_doorhanger() {
  await SpecialPowers.pushPrefEnv({
    set: [["media.webspeech.recognition.enable", true]],
  });

  await BrowserTestUtils.withNewTab(PAGE, async browser => {
    let popupShown = BrowserTestUtils.waitForEvent(
      PopupNotifications.panel,
      "popupshown"
    );

    await triggerInstall(browser);
    await popupShown;

    let notification = PopupNotifications.getNotification(
      "speech-recognition-model-download",
      browser
    );
    ok(notification, "speech-recognition-model-download notification exists");

    // Dismiss without downloading.
    let popupHidden = BrowserTestUtils.waitForEvent(
      PopupNotifications.panel,
      "popuphidden"
    );
    notification.remove();
    await popupHidden;
  });

  await SpecialPowers.popPrefEnv();
});

add_task(async function test_install_not_now_resolves_false() {
  await SpecialPowers.pushPrefEnv({
    set: [["media.webspeech.recognition.enable", true]],
  });

  await BrowserTestUtils.withNewTab(PAGE, async browser => {
    let popupShown = BrowserTestUtils.waitForEvent(
      PopupNotifications.panel,
      "popupshown"
    );

    let installResult = SpecialPowers.spawn(browser, [], async () => {
      content.document.notifyUserGestureActivation();
      return content.SpeechRecognition.install({
        langs: ["en-US"],
        processLocally: true,
      });
    });

    await popupShown;

    let notification = PopupNotifications.getNotification(
      "speech-recognition-model-download",
      browser
    );
    ok(notification, "Notification present before dismissal");

    // Click the secondary ("Not Now") action.
    notification.secondaryActions[0].callback();

    let result = await installResult;
    is(result, false, "install() resolves false when user dismisses");
  });

  await SpecialPowers.popPrefEnv();
});

// install() must skip the permission prompt entirely once the model is
// already installed: there is nothing to download, so nothing to consent to.
add_task(async function test_install_skips_prompt_when_already_installed() {
  // Mock backend; auto-allow the first install so the model becomes installed.
  await SpecialPowers.pushPrefEnv({
    set: [
      ["media.webspeech.recognition.enable", true],
      ["media.webspeech.recognition.testing", true],
      ["media.webspeech.recognition.model-download.prompt.testing", true],
      ["media.navigator.permission.disabled", true],
    ],
  });

  await BrowserTestUtils.withNewTab(PAGE, async browser => {
    await SpecialPowers.spawn(browser, [], () => {
      content.document.notifyUserGestureActivation();
      return content.SpeechRecognition.install({
        langs: ["en-US"],
        processLocally: true,
      });
    });
    let status = await SpecialPowers.spawn(browser, [], () =>
      content.SpeechRecognition.available({
        langs: ["en-US"],
        processLocally: true,
      })
    );
    is(status, "available", "model reports available after first install");
  });

  // Real prompt path. The model is now installed, so a second install() must
  // resolve true without ever showing the permission prompt.
  await SpecialPowers.pushPrefEnv({
    set: [
      ["media.webspeech.recognition.model-download.prompt.testing", false],
      ["media.navigator.permission.disabled", false],
    ],
  });

  await BrowserTestUtils.withNewTab(PAGE, async browser => {
    let popupShown = false;
    let listener = () => {
      popupShown = true;
    };
    PopupNotifications.panel.addEventListener("popupshown", listener);

    let installed = await SpecialPowers.spawn(browser, [], () => {
      content.document.notifyUserGestureActivation();
      return content.SpeechRecognition.install({
        langs: ["en-US"],
        processLocally: true,
      });
    });

    PopupNotifications.panel.removeEventListener("popupshown", listener);

    ok(installed, "install() resolves true for an already-installed model");
    ok(
      !popupShown,
      "install() must not show the prompt when already installed"
    );
  });

  await SpecialPowers.popPrefEnv();
  await SpecialPowers.popPrefEnv();
});
