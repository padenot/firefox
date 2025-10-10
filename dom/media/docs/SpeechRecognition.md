# Gecko Speech Recognition implementation architecture

This document summarizes the architecture, call flows, threading model, object
lifetimes, and known gaps for the new SpeechRecognition implementation, that is
able to perform recognition locally, without relying on an external service.

For now, it is intended to help reviewing the implementation, and will be turned
into an architecture overview document prior to landing.

## Architecture

Here's an high level outline of the components of this system, :

Content process:

- `SpeechRecognition` (main thread): implementation of the "recognition" side of
  the [Web Speech
  API](https://webaudio.github.io/web-speech-api/#speechreco-section)
- `SpeechRecognitionBackend` (main thread, real-time audio thread, IPC thread):
  audio ingestion and processing, audibility detection, IPC communication with
  the utility process (sending audio, receiving transcript).
- `HWInferenceManagerChild` + `SpeechRecognitionChild`: content-side IPDL
  actors managing a direct channel to the utility process.

Utility Process (HWInference)
- `HWInferenceManagerParent`: manager for per-session recognition actors.
- `SpeechRecognitionParent` (dedicated thread, per session): receives audio from content, runs
  speech recognition on a dedicated thread, sends transcription results back to the content process.
- `whisper.cpp` library (via `LlamaRuntimeLinker`, loading it dynamically from
  `libmozinference`), backed by `libggml`, optionally using GPU acceleration,
  for now only macOS. `SpeechRecognitionParent` implements the actual speech
  recognition using the library.
- `HWInferenceChild`: utility→main process bridge for speech recognition model
  availability/installation requests, and to get a file descriptor passed down
  from the main process.

Main Process:

- `ContentParent::RecvRequestHWInferenceConnection`: brokers a direct endpoint
  for `PHWInferenceManager` content↔utility IPC channel.
- `UtilityProcessManager`: launches/binds the HWInference utility process and
  returns endpoints.
- `HWInferenceParent` (main-process side of `PHWInference`): receives calls for
  model availability/install checks, and calls to get a file descriptor down to
  the HWInference process directly via IPC. Uses `nsIMLModelHub` to call into
  `ModelHub`.
- `nsIMLModelHub`: thin XPCOM component that wraps `ModelHub`, allowing its use
  from native code.

## Design choices

### The `HWInference` process

`HWInference` is a new utility process that doesn't run JavaScript. Its main job
is to run inference, using libraries that leverage the hardware of the device. Its
sandbox policy resembles that of the GPU process, but it doesn't have access to
the display server, or have access to things like fonts, or other special system
calls or capabilities related to rendering. It only does computations: it
receives some input (in our case, audio data) and uses the model file to perform
inference, and produce some output (in our case, timed text fragments). It is
only started when needed, and closed quickly when not needed anymore.

It delegates all model management tasks to the
[ModelHub](https://searchfox.org/firefox-main/source/toolkit/components/ml/content/ModelHub.sys.mjs),
that it can call via IPC. This includes model availability checks, download
(`ModelHub` handles caching). Finally, it can acquire a handle to a model file,
using a FileDescriptor passed via IPC. This is important, because model files can be quite big, we
absolutely want to skip any copy, and need to be able to `mmap` some models
(notably mixture of experts models), for significant memory footprint gains.

This process will also be used for tasks unrelated to speech recognition:
ONNX Runtime (for non-LLM type inference) and `llama.cpp` (for LLM-type
inference) will be taught to run inside the `HWInference` process, to be able to
use hardware acceleration.

Since it doesn't run javascript, we'll be able to tighten the sandbox on macOS
by making it a different executable, relinguising the capability to mark pages
as executable for JITing code.

### `whisper.cpp`

`whisper.cpp` is a C++ library that can perform speech recognition using models
of the Whisper family. It uses `libggml` underneath for the actual computations
(accelerated or not). It is a good choice because we already vendor `libggml`,
as it is the backend of `llama.cpp`, that we use for e.g. text summarization.

It consists in a single `cpp` file and two header files most of the heavy
lifting is done by `libggml`.

I also plan to experiment with Nvidia Parakeet, a more recent contender, that
can run on ONNX (that we also have). I have read that Parakeet is faster, higher
quality, but it doesn't have models for as many languages as whisper.cpp.

In this patch set, the Metal backend (macOS) has been vendored. The Vulkan
backend (Windows, Linux, Android) will be worked on in a second stage. The CPU
backend works on all platforms.

#### The speech recognition itself

This is best explained in comments in the code, see
`SpeechRecognitionParent::ProcessAudioOnBackgroundThread` in
`SpeechRecognitionParent.cpp`.

This will have to be tuned and I have made most parameters tweakable using prefs
for this purpose. I also want to experiment with much longer steps (up to 30s),
and use token (not string) deduplication.

#### The models

I have uploaded a few models to our bucket, an english-only model, and a
multilingual model. I expect that more models will be added in the future, both
with different performance characteristics, but also containing different
languages, and with different capabilities, such as token timestamping,
diarisation, punctuation correctness, etc. Consequently, the language validation
is currently minimal: english goes to the english model, everything else to the
other model.

Our bucket also contains a Voice Activity Detection (VAD) model (Silero VAD),
that can be used to detect speech activity in audio data, but I haven't wired it
yet.

### Lifetimes, thread model

#### Content process

The audio is produced by a real-time thread. It is best to do as little as
possible on it. Consequently, only downmixing to mono (that is almost free) is
done there, and the audio is immediately enqueued to a wait-free ring buffer.

A dedicated thread (called `SpeechResampler`) runs every 500ms or so, resamples
the audio to the model's sampling rate (constant at 16kHz), and dispatches a
chunk of audio (~32kB) to the `HWInference` process. It is started on
recognition start, stopped on recognition stop or abort.

A **single** thread per content process handles the IPC from the content process
to the HWInference process. Because the `SpeechRecognition` object has both
static and instance methods, all the IPC calls run on this thread. This thread
is shut down if not used.

`PSpeechRecognition` actors can be created for two reasons:
- transient instances are created and shortly after deleted for
  available/install calls. A number of those instances can be active at once,
  e.g. when available/install calls are spammed.
- long-running instances are created for speech recognition. They are kept alive
  until the user stops the recognition. A single instance can be active at once
  (to be relaxed when we allow concurrent speech recognition, after performance
  testing).

#### HWInference process

`SpeechRecognitionParent` handles most of the recognition process. It has a
dedicated thread, started during speech recognition session init, closed during
speech recognition session shutdown. It essentially loops, dequeues audio,
massages it a little bit and feeds it to `whisper.cpp`.

Its lifetime is dictated by the content process, and only a single session can
be active at once in Firefox (for now, prior to performance testing, this
matches Chrome).

Whisper objects have the same lifetime as a recognition session. The
initialization requires IPC and is highly asynchronous, to acquire the model
file, but after the init phrase, everything happens on the dedicated thread,
except appending to the SPSC ring buffer, since the audio comes from IPC.

#### Main process

The main process is only used to create the `HWInference` process, and to
interact with ModelHub.

### Threads used

**Content process**

- `Main thread` for the implementation of the DOM api
- `MediaTrackGraph` real-time audio thread produces audio data
- Dedicated `SpeechIPC` thread to use the `PSpeechRecognition` actor
  from a stable thread, both for static calls and instance calls
- Dedicated `SpeechResampler` thread to consume audio data, resample the audio,
  dispatches to `SpeechIPC` that dispatches to `SpeechIPC`

**HWInference process**

- Main thread receives commands and audio via IPC, produces audio into a ring buffer
- Dedicated `Whisper` thread receives command, initialize recognition, consumes
  audio from the ring buffer, performs inference

**Parent process**

- No new threads

## Sequence diagrams

This section shows the flow of events and interactions between the different
components involved in the SpeechRecognition process. It covers a simple
scenario: calling `available()` with a language identifier, calling `install()`
with the same language identifier, then starting recognition from a
`MediaStreamTrack`.

### Checking model availability

This diagram covers shows the sequence of events that happens when calling:

```js
SpeechRecognition.available({lang: ["en-US"], processLocally: true});
```

```{mermaid}
sequenceDiagram
  autonumber

  box Content Process
    participant JS as Script
    participant SR as SpeechRecognition
    participant BE as SpeechRecognitionBackend
    participant CC as ContentChild
    participant HMC as HWInferenceManagerChild
    participant SRC as PSpeechRecognitionChild
  end

  box Main Process
    participant CP as ContentParent
    participant UPM as UtilityProcessManager
    participant HWP as HWInferenceParent
    participant NSIMLMH as nsIMLModelHub
    participant MH as ModelHub
  end

  box Utility Process (HWInference)
    participant HMP as HWInferenceManagerParent
    participant SRP as SpeechRecognitionParent
    participant HWC as HWInferenceChild
  end

  JS->>SR: SpeechRecognition.available({lang: "en-US", processLocally: true})
  SR->>BE: SpeechRecognitionBackend::Available(langs)
  BE->>CC: RequestHWInferenceConnection()
  CC->>CP: PContent::RequestHWInferenceConnection
  CP->>UPM: StartContentHWInferenceManager(...)
  UPM-->>CC: Endpoint<PHWInferenceManagerChild>
  CC->>HMC: OpenForProcess(endpoint)
  BE->>HMC: Create PSpeechRecognitionChild (transient)
  HMC->>HMP: PSpeechRecognition(alloc)
  BE->>SRC: SendIsModelAvailable(langs)
  SRC->>SRP: SendIsModelAvailable
  SRP->>HWC: PHWInferenceChild::IsModelAvailable(model,rev,file)
  HWC->>HWP: PHWInferenceChild::SendIsModelAvailable
  HWP->>NSIMLMH: "nsIMLModelHub.isModelAvailable(...)"
  NSIMLMH->>MH: "ModelHub.available(...)"

  MH-->>NSIMLMH: bool
  NSIMLMH-->>HWP: bool
  HWP-->>HWC: bool
  SRP-->>SRC: bool
  SRC-->>BE: bool
  BE->>SRC: `Send__delete__`
  BE-->>SR: Promise resolves: available
  Note over JS: Local speech recognition model available for en_US
  SR-->>JS: Promise resolves: available
```

### Downloading and installing a model

This diagram skips the HWInference process creation, because it is the same as
in the previous diagram. It is what happens when running:

```js
SpeechRecognition.available({lang: ["en-US"]});
```

```{mermaid}
sequenceDiagram
  autonumber

  box Content Process
    participant JS as Script
    participant SR as SpeechRecognition
    participant BE as SpeechRecognitionBackend
    participant HMC as HWInferenceManagerChild
    participant SRC as SpeechRecognitionChild
  end

  box Utility Process (HWInference)
    participant SRP as SpeechRecognitionParent
    participant HWC as HWInferenceChild
  end

  box Main Process
    participant HWP as HWInferenceParent
    participant NSIMLMH as nsIMLModelHub
    participant MH as ModelHub
  end

  JS->>SR: SpeechRecognition.install({lang: "en-US"})
  SR->>BE: ::Install(langs)
  BE->>HMC: Create SpeechRecognitionChild (transient)
  BE->>SRC: ::SendInstallModels(langs)
  SRC->>SRP: ::SendInstallModels
  SRP->>HWC: ::InstallModel
  HWC->>HWP: ::SendInstallModel
  HWP->>NSIMLMH: downloadModel(...)
  NSIMLMH->>MH: getModelDataAsFile(...)
  activate MH
  MH--)NSIMLMH: progress callback
  NSIMLMH--)HWP: progress callback
  MH--)NSIMLMH: Download complete
  deactivate MH
  NSIMLMH-->>HWP: download success/fail
  HWP-->>HWC: bool
  HWC-->>SRP: bool
  SRP-->>SRC: bool
  SRC-->>BE: bool
  BE-->>SR: Promise resolves(bool)
  Note over JS: Speech recognition model downloaded and installed
  SR-->>JS: Promise resolves(bool)
```

### Starting recognition and processing audio

This is what happens after running `start(...)` on a `SpeechRecognition`
instance that is processing locally, passing it a `MediaStreamTrack`. Again, the
initial process creation isn't repeted and is similar to the first diagram.

There are three loops running in parallel at with different interval, in
different process and with different thread priorities in this diagram:

```{mermaid}
sequenceDiagram
  autonumber

  box Content Process
    participant JS as Script
    participant SR as SpeechRecognition
    participant MTG as MediaTrackGraph
    participant BE as SpeechRecognitionBackend
    participant HMC as HWInferenceManagerChild
    participant SRC as SpeechRecognitionChild
  end

  box Utility Process (HWInference)
    participant SRP as SpeechRecognitionParent
    participant HWC as HWInferenceChild
    participant WLIB as whisper.cpp
  end

  box Main Process
    participant HWP as HWInferenceParent
    participant MH as ModelHub
  end

  JS->>SR: "start([track])"
  SR->>SR: "Validate track or getUserMedia"
  SR->>BE: "new Backend, Start()"
  BE->>HMC: "Create SpeechRecognitionChild"
  BE->>SRC: "SendInit(lang, phrases)"
  SRC->>SRP: SendInit
  SRP->>HWC: PHWInference::GetModelFile
  HWC->>HWP: RecvGetModelFile
  HWP->>MH: getModelFileAsBlob(...)
  MH-->>HWP: Blob
  HWP-->>HWC: FileDescriptor
  HWC-->>SRP: FileDescriptor
  SRP->>SRP: `FileDescriptor` to `FILE*`
  SRP->>WLIB: `whisper_init(FILE* model, ...)`
  activate WLIB
  WLIB->>WLIB: `fread`, compile shaders, etc.
  WLIB-->>SRP: init OK
  deactivate WLIB
  SRP-->>SRC: Init resolved true
  SRC-->>BE: Init resolved true
  BE->>BE: Start resampling thread
  SRP->>SRP: Start Whisper thread
  loop Audio capture loop, real-time thread, every ~3 to 20ms
    MTG->>MTG: SpeechTrackListener::NotifyQueuedChanges
    MTG->>BE: SpeechRecognitionBackend::DataCallback
    BE->>BE: Downmix, enqueue 128-frame chunks on real-time thread
  end
  loop Speech resampling loop, SpeechResampling thread, every ~500ms (configurable)
    BE->>BE: Dequeues 500ms, resample to 16Khz on Resampling thread
    BE->>SRC: SendAudioDataViaIPC(16kHz f32)
    SRC->>SRP: SendProcessAudioData(16Khz f32)
    SRP->>SRP: Enqueue
  end

  loop Whisper recognition loop, every ~3000ms (configurable)
    SRP->>SRP: Dequeue
    SRP->>WLIB: whisper_full()
    WLIB-->>SRP: timed text fragment
    SRP-->>SRC: OnRecognitionResult(text, interim or final)
    SRC-->>BE: Result callback
    BE-->>SR: Dispatch result event
    Note over JS: recognized text fragments received by script
    SR-->>JS: SpeechRecognitionResult
  end
```

## Open Issues / Not Quite Done / Limitations

### Spec

The spec is really not in a good shape, and I'll take advantage of TPAC to agree
with other implementors on the fixes. I'm still figuring how all the items I
want addressed. I had to look at Chromium's code to understand certain behaviour.

`stop` then `start`ing again isn't implemented per spec for now, but that is a
simple fix.

Also, the spec isn't clear how to time recognition events. We need to lock down
the language in terms of clock domain. We have the capability to have per token
timestamps, but we aren't sending the timing information currently.

### Testing

All non-trivial WPTs are manual tests. I need to write a mock backend for all
this, that will exercise all the IPC, ingest audio, and send back fake text
fragments. I have been doing manual testing for now.

### `"speechstart"`/`"speechend"` events

Content-side callbacks are wired (`SpeechRecognitionChild::RecvOnSpeechChange` →
backend → DOM), but `SpeechRecognitionParent` never calls
`SendOnSpeechChange(...)`. One remaining task is to add some code to use a
minuscule VAD (Voice Activity Detection) model and get timing of speech
start/end. This is supported upstream in `whisper.cpp`.

### Global concurrency limit scope

The system currently only supports a single active session via static
`sActiveSession` across the entire `HWInference` process. Chrome does the same.
We will be able to relax this when we understand better the performance story
when there is no hardware acceleration. Having a bunch of recognition sessions
running concurrently is fine when there is hardware acceleration, granted the
same model is used for all session (or there is otherwise enough memory
available).

### Error handline / propagation

`HandleRecognitionErrorFromBackend` maps only `"concurrent-session"` to
`service-not-allowed`, defaulting others to `network`. This will be expanded
and clarified.

### Language→model mapping

`LanguagesToModelIdentifier` uses only the first language; maps `en`/`en-US`
to `ggml-small.en` and others to `ggml-large-v3-turbo-q8_0`. While the
question of model selection and fine tuning is more or less outside of the
scope of this patch set, I plan to add more models (much smaller, more
specialized with different variants, etc.) prior to landing

## No user prompt for model downloading

I'll wire up something like our translation model download.

### Phrase boost

Actual "boosting" implementation using something like logits post-processing.
For now, we collect the phrases into a single string and pass it to
`whisper.cpp` as the initial prompt. This works but could be more granular.

### Hardware acceleration on non-macOS

Still needs to be done, CPU-based inference works well though.
