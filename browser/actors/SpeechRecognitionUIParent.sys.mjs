/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Shows a notification bar when a page tried to use on-device speech
// recognition while it is disabled in AI Controls. It offers to opt in to just
// this feature inline, leaving any other AI Controls state untouched, and to
// deep-link to the AI Controls settings.
export class SpeechRecognitionUIParent extends JSWindowActorParent {
  receiveMessage(message) {
    if (message.name == "SpeechRecognitionUI:Blocked") {
      this.#showBlockedNotification();
    }
  }

  #showBlockedNotification() {
    let browser = this.browsingContext.top.embedderElement;
    if (!browser) {
      return;
    }
    let notificationBox = browser.getTabBrowser().getNotificationBox(browser);
    let value = "speech-recognition-ai-blocked";
    if (notificationBox.getNotificationWithValue(value)) {
      return;
    }

    notificationBox.appendNotification(
      value,
      {
        label: { "l10n-id": "speech-recognition-ai-blocked-infobar-message" },
        priority: notificationBox.PRIORITY_INFO_HIGH,
      },
      [
        {
          supportPage: "speech-recognition-firefox",
        },
        {
          "l10n-id": "speech-recognition-ai-blocked-infobar-enable",
          primary: true,
          callback: () => {
            Services.prefs.setCharPref(
              "browser.ai.control.speechRecognition",
              "enabled"
            );
          },
        },
        {
          "l10n-id": "speech-recognition-ai-blocked-infobar-settings",
          callback: () => {
            this.browsingContext.topChromeWindow?.openPreferences(
              "ai-speechRecognition"
            );
          },
        },
      ]
    );
  }
}
