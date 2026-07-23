/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Observes the "speech-recognition-ai-blocked" notification dispatched by
// SpeechRecognition (dom/media/webspeech/recognition/SpeechRecognition.cpp)
// when a page tries to use on-device speech recognition while it is disabled in
// AI Controls, and forwards it to the parent so a notification bar can be shown.
export class SpeechRecognitionUIChild extends JSWindowActorChild {
  observe(subject, topic) {
    if (topic == "speech-recognition-ai-blocked") {
      this.sendAsyncMessage("SpeechRecognitionUI:Blocked");
    }
  }
}
