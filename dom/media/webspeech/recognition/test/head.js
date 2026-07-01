"use strict";

const DEFAULT_AUDIO_SAMPLE_FILE = "hello.ogg";
const SPEECH_RECOGNITION_TEST_REQUEST_EVENT_TOPIC =
  "SpeechRecognitionTest:RequestEvent";
const SPEECH_RECOGNITION_TEST_END_TOPIC = "SpeechRecognitionTest:End";

// Always disable autoplay blocking in tests that drive an AudioContext:
// without this, playback/captured tracks can be silently gated even after
// resume(), which looks identical to a genuine audio-pipeline bug.
const SPEECH_TEST_AUTOPLAY_PREFS = [
  ["media.autoplay.default", 0],
  ["media.autoplay.blocking_policy", 0],
];

// In headless CI, a `new AudioContext()` is created suspended and does NOT
// auto-start; without an explicit resume() the graph never runs and every
// captured track is silent. Always resume explicitly instead of assuming a
// user gesture or autoplay prefs are enough. resume() itself has been
// observed to never settle on flaky headless audio backends (pipewire), so
// bound the wait and fail fast rather than hang until the harness timeout.
async function createResumedAudioContext({ timeoutMs = 5000 } = {}) {
  const ctx = new AudioContext();
  await Promise.race([
    ctx.resume(),
    new Promise((_, reject) => setTimeout(
      () => reject(new Error(
        `AudioContext.resume() did not settle within ${timeoutMs}ms ` +
        `(state=${ctx.state}); likely a stuck headless audio backend.`)),
      timeoutMs)),
  ]);
  if (ctx.state !== "running") {
    throw new Error(`AudioContext failed to resume: state=${ctx.state}`);
  }
  return ctx;
}

// Peak and mean FFT magnitude over the current frame: the cheap way to tell
// "is there actually sound on this track" from "the track merely exists".
function AudioStreamAnalyser(ac, stream) {
  this.audioContext = ac;
  this.analyser = this.audioContext.createAnalyser();
  this.analyser.smoothingTimeConstant = 0.2;
  this.analyser.fftSize = 1024;
  this.sourceNodes = [];
  this.connectTrack = t => {
    let source = this.audioContext.createMediaStreamSource(new MediaStream([t]));
    this.sourceNodes.push(source);
    source.connect(this.analyser);
  };
  stream.getAudioTracks().forEach(t => this.connectTrack(t));
  this.data = new Uint8Array(this.analyser.frequencyBinCount);
}
AudioStreamAnalyser.prototype.getByteFrequencyData = function() {
  this.analyser.getByteFrequencyData(this.data);
  return this.data;
};
AudioStreamAnalyser.prototype.levels = function() {
  const d = this.getByteFrequencyData();
  let max = 0, sum = 0;
  for (let i = 0; i < d.length; i++) { sum += d[i]; if (d[i] > max) max = d[i]; }
  return { max, avg: +(sum / d.length).toFixed(1) };
};
AudioStreamAnalyser.prototype.disconnect = function() {
  this.sourceNodes.forEach(n => n.disconnect());
};

// Polls a track for real audio for up to timeoutMs and throws immediately
// once it's clear nothing is flowing, instead of leaving a caller to hang
// waiting on a downstream event (e.g. onaudiostart) that will never fire.
// Returns the peak level observed.
async function waitForAudioFlowing(ctx, stream, { timeoutMs = 3000, pollMs = 100 } = {}) {
  const analyser = new AudioStreamAnalyser(ctx, stream);
  try {
    const start = performance.now();
    let maxLevel = 0;
    while (performance.now() - start < timeoutMs) {
      const lvl = analyser.levels();
      if (lvl.max > maxLevel) {
        maxLevel = lvl.max;
      }
      if (maxLevel > 0) {
        return maxLevel;
      }
      await new Promise(r => setTimeout(r, pollMs));
    }
    throw new Error(
      `No audio flowing into track after ${timeoutMs}ms (ctx.state=${ctx.state}). ` +
      "Check AudioContext.resume(), autoplay-block prefs, and the audio source."
    );
  } finally {
    analyser.disconnect();
  }
}

var errorCodes = {
  NO_SPEECH: "no-speech",
  ABORTED: "aborted",
  AUDIO_CAPTURE: "audio-capture",
  NETWORK: "network",
  NOT_ALLOWED: "not-allowed",
  SERVICE_NOT_ALLOWED: "service-not-allowed",
  BAD_GRAMMAR: "bad-grammar",
  LANGUAGE_NOT_SUPPORTED: "language-not-supported",
};

var Services = SpecialPowers.Services;

function EventManager(sr) {
  var self = this;
  var nEventsExpected = 0;
  self.eventsReceived = [];

  var allEvents = [
    "audiostart",
    "soundstart",
    "speechstart",
    "speechend",
    "soundend",
    "audioend",
    "result",
    "nomatch",
    "error",
    "start",
    "end",
  ];

  var eventDependencies = {
    speechend: "speechstart",
    soundend: "soundstart",
    audioend: "audiostart",
  };

  var isDone = false;

  // set up grammar
  var sgl = new SpeechGrammarList();
  sgl.addFromString("#JSGF V1.0; grammar test; public <simple> = hello ;", 1);
  sr.grammars = sgl;

  // AUDIO_DATA events are asynchronous,
  // so we queue events requested while they are being
  // issued to make them seem synchronous
  var isSendingAudioData = false;
  var queuedEventRequests = [];

  // register default handlers
  for (var i = 0; i < allEvents.length; i++) {
    (function (eventName) {
      sr["on" + eventName] = function (evt) {
        var message = "unexpected event: " + eventName;
        if (eventName == "error") {
          message += " -- " + evt.message;
        }

        ok(false, message);
        if (self.doneFunc && !isDone) {
          isDone = true;
          self.doneFunc();
        }
      };
    })(allEvents[i]);
  }

  self.expect = function EventManager_expect(eventName, cb) {
    nEventsExpected++;

    sr["on" + eventName] = function (evt) {
      self.eventsReceived.push(eventName);
      ok(true, "received event " + eventName);

      var dep = eventDependencies[eventName];
      if (dep) {
        ok(
          self.eventsReceived.includes(dep),
          eventName + " must come after " + dep
        );
      }

      cb && cb(evt, sr);
      if (
        self.doneFunc &&
        !isDone &&
        nEventsExpected === self.eventsReceived.length
      ) {
        isDone = true;
        self.doneFunc();
      }
    };
  };

  self.start = function EventManager_start() {
    isSendingAudioData = true;
    var audioTag = document.createElement("audio");
    audioTag.src = self.audioSampleFile;

    var stream = audioTag.mozCaptureStreamUntilEnded();
    audioTag.addEventListener("ended", function () {
      info("Sample stream ended, requesting queued events");
      isSendingAudioData = false;
      while (queuedEventRequests.length) {
        self.requestFSMEvent(queuedEventRequests.shift());
      }
    });

    audioTag.play();
    sr.start(stream);
  };

  self.requestFSMEvent = function EventManager_requestFSMEvent(eventName) {
    if (isSendingAudioData) {
      info(
        "Queuing event " + eventName + " until we're done sending audio data"
      );
      queuedEventRequests.push(eventName);
      return;
    }

    info("requesting " + eventName);
    Services.obs.notifyObservers(
      null,
      SPEECH_RECOGNITION_TEST_REQUEST_EVENT_TOPIC,
      eventName
    );
  };

  self.requestTestEnd = function EventManager_requestTestEnd() {
    Services.obs.notifyObservers(null, SPEECH_RECOGNITION_TEST_END_TOPIC);
  };
}

function buildResultCallback(transcript) {
  return function (evt) {
    is(evt.results[0][0].transcript, transcript, "expect correct transcript");
  };
}

function buildErrorCallback(errcode) {
  return function (err) {
    is(err.error, errcode, "expect correct error code");
  };
}

function performTest(options) {
  var prefs = options.prefs;

  prefs.unshift(
    ["media.webspeech.recognition.enable", true],
    ["media.webspeech.test.enable", true]
  );

  SpecialPowers.pushPrefEnv({ set: prefs }, function () {
    var sr;
    if (!options.webkit) {
      sr = new SpeechRecognition();
    } else {
      sr = new webkitSpeechRecognition();
      var grammar = new webkitSpeechGrammar();
      var speechrecognitionlist = new webkitSpeechGrammarList();
      speechrecognitionlist.addFromString("", 1);
      sr.grammars = speechrecognitionlist;
    }
    var em = new EventManager(sr);

    for (var eventName in options.expectedEvents) {
      var cb = options.expectedEvents[eventName];
      em.expect(eventName, cb);
    }

    em.doneFunc = function () {
      em.requestTestEnd();
      if (options.doneFunc) {
        options.doneFunc();
      }
    };

    em.audioSampleFile = DEFAULT_AUDIO_SAMPLE_FILE;
    if (options.audioSampleFile) {
      em.audioSampleFile = options.audioSampleFile;
    }

    em.start();

    for (var i = 0; i < options.eventsToRequest.length; i++) {
      em.requestFSMEvent(options.eventsToRequest[i]);
    }
  });
}
