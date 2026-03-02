/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * The origin of this IDL file is
 * https://webaudio.github.io/web-audio-api/
 *
 * Copyright © 2012 W3C® (MIT, ERCIM, Keio), All Rights Reserved. W3C
 * liability, trademark and document use rules apply.
 */

// https://webaudio.github.io/web-audio-api/#enumdef-audiocontextlatencycategory
enum AudioContextLatencyCategory {
  "balanced",
  "interactive",
  "playback"
};

// https://webaudio.github.io/web-audio-api/#AudioContextOptions
dictionary AudioContextOptions {
  (AudioContextLatencyCategory or double) latencyHint = "interactive";
             float        sampleRate;
};

dictionary AudioTimestamp {
  double contextTime;
  DOMHighResTimeStamp performanceTime;
};

[Pref="dom.webaudio.enabled",
 Exposed=Window]
interface AudioContext : BaseAudioContext {
    [Throws]
    constructor(optional AudioContextOptions contextOptions = {});

    readonly        attribute double               baseLatency;
    readonly        attribute double               outputLatency;
    AudioTimestamp                  getOutputTimestamp();

    [NewObject]
    Promise<undefined> suspend();
    [NewObject]
    Promise<undefined> close();

    [NewObject, Throws]
    MediaElementAudioSourceNode createMediaElementSource(HTMLMediaElement mediaElement);

    [NewObject, Throws]
    MediaStreamAudioSourceNode createMediaStreamSource(MediaStream mediaStream);

    [NewObject, Throws]
    MediaStreamTrackAudioSourceNode createMediaStreamTrackSource(MediaStreamTrack mediaStreamTrack);

    [NewObject, Throws]
    MediaStreamAudioDestinationNode createMediaStreamDestination();

    // Test-only: exposes internals for verifying latencyHint behaviour.
    [ChromeOnly]
    readonly attribute unsigned long callbackBufferSize;
    [ChromeOnly]
    readonly attribute unsigned long requestedLatencyFrames;
    [ChromeOnly]
    readonly attribute unsigned long long mediaTrackGraphId;
};
