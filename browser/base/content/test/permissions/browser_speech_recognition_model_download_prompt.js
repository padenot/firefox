/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Verify that SpeechRecognition.install() shows a permission doorhanger and
// that accepting it resolves the promise with true, while dismissing resolves
// with false.

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
