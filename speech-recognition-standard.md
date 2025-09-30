<h3 id="speechreco-section">The SpeechRecognition Interface</h3>

<p>The speech recognition interface is the scripted web API for controlling a given recognition.</p>
The term "final result" indicates a {{SpeechRecognitionResult}} in which the {{SpeechRecognitionResult/isFinal}} attribute is true.
The term "interim result" indicates a {{SpeechRecognitionResult}} in which the {{SpeechRecognitionResult/isFinal}} attribute is false.

{{SpeechRecognition}} has the following internal slots:

<dl dfn-type=attribute dfn-for="SpeechRecognition">
    : <dfn>[[started]]</dfn>
    ::
        A boolean flag representing whether the speech recognition started. The initial value is <code>false</code>.
</dl>

<dl dfn-type=attribute dfn-for="SpeechRecognition">
    : <dfn>[[processLocally]]</dfn>
    ::
        A boolean flag indicating whether recognition <em class="rfc2119" title="MUST">MUST</em> be performed locally. The initial value is <code>false</code>.
</dl>

<dl dfn-type=attribute dfn-for="SpeechRecognition">
    : <dfn>[[phrases]]</dfn>
    ::
        An {{ObservableArray}} of {{SpeechRecognitionPhrase}} objects representing a list of phrases for contextual biasing. The initial value is a new empty {{ObservableArray}}.
</dl>

<xmp class="idl">
[SecureContext, Exposed=Window]
interface SpeechRecognition : EventTarget {
    constructor();

    // recognition parameters
    attribute SpeechGrammarList grammars;
    attribute DOMString lang;
    attribute boolean continuous;
    attribute boolean interimResults;
    attribute unsigned long maxAlternatives;
    attribute boolean processLocally;
    attribute ObservableArray<SpeechRecognitionPhrase> phrases;

    // methods to drive the speech interaction
    undefined start();
    undefined start(MediaStreamTrack audioTrack);
    undefined stop();
    undefined abort();
    static Promise<AvailabilityStatus> available(SpeechRecognitionOptions options);
    static Promise<boolean> install(SpeechRecognitionOptions options);

    // event methods
    attribute EventHandler onaudiostart;
    attribute EventHandler onsoundstart;
    attribute EventHandler onspeechstart;
    attribute EventHandler onspeechend;
    attribute EventHandler onsoundend;
    attribute EventHandler onaudioend;
    attribute EventHandler onresult;
    attribute EventHandler onnomatch;
    attribute EventHandler onerror;
    attribute EventHandler onstart;
    attribute EventHandler onend;
};

dictionary SpeechRecognitionOptions {
  required sequence<DOMString> langs;
  boolean processLocally = false;
};

enum SpeechRecognitionErrorCode {
    "no-speech",
    "aborted",
    "audio-capture",
    "network",
    "not-allowed",
    "service-not-allowed",
    "language-not-supported",
    "phrases-not-supported"
};

enum AvailabilityStatus {
    "unavailable",
    "downloadable",
    "downloading",
    "available"
};

[SecureContext, Exposed=Window]
interface SpeechRecognitionErrorEvent : Event {
    constructor(DOMString type, SpeechRecognitionErrorEventInit eventInitDict);
    readonly attribute SpeechRecognitionErrorCode error;
    readonly attribute DOMString message;
};

dictionary SpeechRecognitionErrorEventInit : EventInit {
    required SpeechRecognitionErrorCode error;
    DOMString message = "";
};

// Item in N-best list
[SecureContext, Exposed=Window]
interface SpeechRecognitionAlternative {
    readonly attribute DOMString transcript;
    readonly attribute float confidence;
};

// A complete one-shot simple response
[SecureContext, Exposed=Window]
interface SpeechRecognitionResult {
    readonly attribute unsigned long length;
    getter SpeechRecognitionAlternative item(unsigned long index);
    readonly attribute boolean isFinal;
};

// A collection of responses (used in continuous mode)
[SecureContext, Exposed=Window]
interface SpeechRecognitionResultList {
    readonly attribute unsigned long length;
    getter SpeechRecognitionResult item(unsigned long index);
};

// A full response, which could be interim or final, part of a continuous response or not
[SecureContext, Exposed=Window]
interface SpeechRecognitionEvent : Event {
    constructor(DOMString type, SpeechRecognitionEventInit eventInitDict);
    readonly attribute unsigned long resultIndex;
    readonly attribute SpeechRecognitionResultList results;
};

dictionary SpeechRecognitionEventInit : EventInit {
    unsigned long resultIndex = 0;
    required SpeechRecognitionResultList results;
};

// The object representing a speech grammar. This interface has been deprecated and exists in this spec for the sole purpose of maintaining backwards compatibility.
[Exposed=Window]
interface SpeechGrammar {
    attribute DOMString src;
    attribute float weight;
};

// The object representing a speech grammar collection. This interface has been deprecated and exists in this spec for the sole purpose of maintaining backwards compatibility.
[Exposed=Window]
interface SpeechGrammarList {
    constructor();
    readonly attribute unsigned long length;
    getter SpeechGrammar item(unsigned long index);
    undefined addFromURI(DOMString src,
                    optional float weight = 1.0);
    undefined addFromString(DOMString string,
                    optional float weight = 1.0);
};

// The object representing a phrase for contextual biasing.
[SecureContext, Exposed=Window]
interface SpeechRecognitionPhrase {
    constructor(DOMString phrase, optional float boost = 1.0);
    readonly attribute DOMString phrase;
    readonly attribute float boost;
};
</xmp>

<h4 id="speechreco-attributes">SpeechRecognition Attributes</h4>

<dl>
  <dt><dfn attribute for=SpeechRecognition>grammars</dfn> attribute</dt>
  <dd>The grammars attribute stores the collection of SpeechGrammar objects which represent the grammars that are active for this recognition. 
  This attribute does nothing and exists in this spec for the sole purpose of maintaining backwards compatibility.</dd>

  <dt><dfn attribute for=SpeechRecognition>lang</dfn> attribute</dt>
  <dd>This attribute will set the language of the recognition for the request, using a valid BCP 47 language tag. [[!BCP47]]
  If unset it remains unset for getting in script, but will default to use the language of the html document root element and associated hierarchy.
  This default value is computed and used when the input request opens a connection to the recognition service.</dd>

  <dt><dfn attribute for=SpeechRecognition>continuous</dfn> attribute</dt>
  <dd>When the continuous attribute is set to false, the user agent must return no more than one final result in response to starting recognition,
  for example a single turn pattern of interaction.
  When the continuous attribute is set to true, the user agent must return zero or more final results representing multiple consecutive recognitions in response to starting recognition,
  for example a dictation.
  The default value must be false.  Note, this attribute setting does not affect interim results.</dd>

  <dt><dfn attribute for=SpeechRecognition>interimResults</dfn> attribute</dt>
  <dd>Controls whether interim results are returned.
  When set to true, interim results should be returned.
  When set to false, interim results must not be returned.
  The default value must be false. Note, this attribute setting does not affect final results.</dd>

  <dt><dfn attribute for=SpeechRecognition>maxAlternatives</dfn> attribute</dt>
  <dd>This attribute will set the maximum number of {{SpeechRecognitionAlternative}}s per result.
  The default value is 1.</dd>

  <dt><dfn attribute for=SpeechRecognition>processLocally</dfn> attribute</dt>
  <dd>This attribute, when set to true, indicates a requirement that the speech recognition process <em class="rfc2119" title="MUST">MUST</em> be performed locally on the user's device.
  If set to false, the user agent can choose between local and remote processing.
  The default value is false.
  </dd>

  <dt><dfn attribute for=SpeechRecognition>phrases</dfn> attribute</dt>
  <dd>
    The `phrases` attribute provides a list of {{SpeechRecognitionPhrase}} objects to be used for contextual biasing. This is an {{ObservableArray}}, which can be modified like a JavaScript `Array` (e.g., using `push()`).
  </dd>
  <dd>
    The getter steps are to return the value of {{SpeechRecognition/[[phrases]]}}.
  </dd>
</dl>

<p class=issue>The group has discussed whether WebRTC might be used to specify selection of audio sources and remote recognizers.
See <a href="https://lists.w3.org/Archives/Public/public-speech-api/2012Sep/0072.html">Interacting with WebRTC, the Web Audio API and other external sources</a> thread on public-speech-api@w3.org.</p>

<h4 id="speechreco-methods">SpeechRecognition Methods</h4>

<dl>
  <dt><dfn method for=SpeechRecognition>start()</dfn> method</dt>
  <dd>
    Start the speech recognition process, directly from a microphone on the device.
    When invoked, run the following steps:

    1. Let |requestMicrophonePermission| be a boolan variable set to to `true`.
    1. Run the [=start session algorithm=] with |requestMicrophonePermission|.
  </dd>

  <dt><dfn method for=SpeechRecognition>start({{MediaStreamTrack}} audioTrack)</dfn> method</dt>
  <dd>
    Start the speech recognition process, using a {{MediaStreamTrack}}
    When invoked, run the following steps:

    1. Let |audioTrack| be the first argument.
    1. If |audioTrack|'s {{MediaStreamTrack/kind}} attribute is NOT `"audio"`,
        throw an {{InvalidStateError}} and abort these steps.
    1. If |audioTrack|'s {{MediaStreamTrack/readyState}} attribute is NOT
        `"live"`, throw an {{InvalidStateError}} and abort these steps.
    1. Let |requestMicrophonePermission| be `false`.
    1. Run the [=start session algorithm=] with |requestMicrophonePermission|.
  </dd>

  <dt><dfn method for=SpeechRecognition>stop()</dfn> method</dt>
  <dd>The stop method represents an instruction to the recognition service to stop listening to more audio, and to try and return a result using just the audio that it has already received for this recognition.
  A typical use of the stop method might be for a web application where the end user is doing the end pointing, similar to a walkie-talkie.
  The end user might press and hold the space bar to talk to the system and on the space down press the start call would have occurred and when the space bar is released the stop method is called to ensure that the system is no longer listening to the user.
  Once the stop method is called the speech service must not collect additional audio and must not continue to listen to the user.
  The speech service must attempt to return a recognition result (or a nomatch) based on the audio that it has already collected for this recognition.
  If the stop method is called on an object which is already stopped or being stopped (that is, start was never called on it, the <a event for=SpeechRecognition>end</a> or <a event for=SpeechRecognition>error</a> event has fired on it, or stop was previously called on it), the user agent must ignore the call.</dd>

  <dt><dfn method for=SpeechRecognition>abort()</dfn> method</dt>
  <dd>The abort method is a request to immediately stop listening and stop recognizing and do not return any information but that the system is done.
  When the abort method is called, the speech service must stop recognizing.
  The user agent must raise an <a event for=SpeechRecognition>end</a> event once the speech service is no longer connected.
  If the abort method is called on an object which is already stopped or aborting (that is, start was never called on it, the <a event for=SpeechRecognition>end</a> or <a event for=SpeechRecognition>error</a> event has fired on it, or abort was previously called on it), the user agent must ignore the call.</dd>

  <dt><dfn method for=SpeechRecognition>available({{SpeechRecognitionOptions}} options)</dfn> method</dt>
  <dd>
    The {{SpeechRecognition/available}} method returns a {{Promise}} that resolves to a {{AvailabilityStatus}} indicating the recognition availability matching the {{SpeechRecognitionOptions}} argument.
    Access to this method is gated behind the [=policy-controlled feature=] "on-device-speech-recognition", which has a [=policy-controlled feature/default allowlist=] of <code>[=default allowlist/'self'=]</code>.

    When invoked, run these steps:
    1. Let <var>promise</var> be <a>a new promise</a>.
    1. Run the <a>availability algorithm</a> with <var>options</var> and <var>promise</var>. If it returns an exception, throw it and abort these steps.
    1. Return <var>promise</var>.
  </dd>

  <dt><dfn method for=SpeechRecognition>install({{SpeechRecognitionOptions}} options)</dfn> method</dt>
  <dd>
    The {{SpeechRecognition/install}} method attempts to install speech recognition language packs for all languages specified in `options.langs`.
    It returns a {{Promise}} that resolves to a {{boolean}}.
    The promise resolves to `true` when all installation attempts for requested and supported languages succeed (or the languages were already installed).
    The promise resolves to `false` if `options.langs` is empty, if not all of the requested languages are supported, or if any installation attempt for a supported language fails.
    Access to this method is gated behind the [=policy-controlled feature=] "on-device-speech-recognition", which has a [=policy-controlled feature/default allowlist=] of <code>[=default allowlist/'self'=]</code>.

    When invoked, run these steps:
    1. If the [=current settings object=]'s [=relevant global object=]'s [=associated Document=] is NOT [=fully active=], throw an {{InvalidStateError}} and abort these steps.
    1. If any <var>lang</var> in {{SpeechRecognitionOptions/langs}} of <var>options</var> is not a valid [[!BCP47]] language tag, throw a {{SyntaxError}} and abort these steps.
    1. If the on-device speech recognition language pack for any <var>lang</var> in {{SpeechRecognitionOptions/langs}} of <var>options</var> is unsupported, return a resolved {{Promise}} with false and skip the rest of these steps.
    1. Let <var>promise</var> be <a>a new promise</a>.
    1. For each <var>lang</var> in {{SpeechRecognitionOptions/langs}} of <var>options</var>, initiate the download of the on-device speech recognition language for <var>lang</var>.
        <p class=note>
          Note: The user agent can prompt the user for explicit permission to download the on-device speech recognition language pack.
        </p>
    1. [=Queue a task=] on the [=relevant global object=]'s [=task queue=] to run the following step:
        - When the download of all languages specified by {{SpeechRecognitionOptions/langs}} of <var>options</var> succeeds, resolve <var>promise</var> with <code>true</code>, otherwise resolve it with <code>false</code>.
            <p class="note">
              Note: The <code>false</code> resolution of the Promise does not indicate the specific cause of failure. User agents are encouraged to provide more detailed information about the failure in developer tools console messages. However, this detailed error information is not exposed to the script.
            </p>
    1. Return <var>promise</var>.
        <p class=note>
          {{SpeechRecognitionOptions/processLocally}} of <var>options</var> is not used in this algorithm.
        </p>
  </dd>

</dl>

<h4 id="availability-status-values">AvailabilityStatus Enum Values</h4>
<p>The {{AvailabilityStatus}} enum indicates the availability of speech recognition capabilities. Its values are:</p>
<dl>
  <dt><dfn enum-value for="AvailabilityStatus">"unavailable"</dfn></dt>
  <dd>Indicates that speech recognition is not available for the specified language(s) and processing preference.
  If {{SpeechRecognitionOptions/processLocally}} of <var>options</var> is `true`, this means on-device recognition for the language is not supported by the user agent.
  If {{SpeechRecognitionOptions/processLocally}} of <var>options</var> is `false`, it means neither local nor remote recognition is available for at least one of the specified languages.</dd>

  <dt><dfn enum-value for="AvailabilityStatus">"downloadable"</dfn></dt>
  <dd>Indicates that on-device speech recognition for the specified language(s) is supported by the user agent but not yet installed. It can potentially be installed using the {{SpeechRecognition/install()}} method. This status is primarily relevant when {{SpeechRecognitionOptions/processLocally}} of <var>options</var> is true.</dd>

  <dt><dfn enum-value for="AvailabilityStatus">"downloading"</dfn></dt>
  <dd>Indicates that on-device speech recognition for the specified language(s) is currently in the process of being downloaded. This status is primarily relevant when {{SpeechRecognitionOptions/processLocally}} of <var>options</var> is true.</dd>

  <dt><dfn enum-value for="AvailabilityStatus">"available"</dfn></dt>
  <dd>Indicates that speech recognition is available for all specified language(s) and the given processing preference.
  If {{SpeechRecognitionOptions/processLocally}} of <var>options</var> is true, this means on-device recognition is installed and ready.
  If {{SpeechRecognitionOptions/processLocally}} of <var>options</var> is false, it means recognition (which could be local or remote) is available.</dd>
</dl>

<p>When the  <dfn>availability algorithm</dfn> with <var>options</var> and <var>promise</var> is invoked, the user agent MUST run the following steps:
1. If the [=current settings object=]'s [=relevant global object=]'s [=associated Document=] is NOT [=fully active=], throw an {{InvalidStateError}} and abort these steps.
1. Let <var>langs</var> be {{SpeechRecognitionOptions/langs}} of <var>options</var>.
1. If any <var>lang</var> in <var>langs</var> is not a valid [[!BCP47]] language tag, throw a {{SyntaxError}} and abort these steps.
1. If {{SpeechRecognitionOptions/processLocally}} of <var>options</var> is `false`:
    1. If <var>langs</var> is an empty sequence, let <var>status</var> be {{AvailabilityStatus/unavailable}}.
    1. Else if speech recognition (which may be remote) is available for all <var>language</var> in <var>langs</var>, let <var>status</var> be {{AvailabilityStatus/available}}.
    1. Else, let <var>status</var> be {{AvailabilityStatus/unavailable}}.
1. If {{SpeechRecognitionOptions/processLocally}} of <var>options</var> is `true`:
      <ol type=a>
        <li>If <var>langs</var> is an empty sequence, let <var>status</var> be {{AvailabilityStatus/unavailable}}.</li>
        <li>Else:
          <ol type=i>
            <li>Let <var>finalStatus</var> be {{AvailabilityStatus/available}}.</li>
            <li>For each <var>language</var> in <var>langs</var>:
              <ol>
                <li>Let <var>currentLanguageStatus</var>.</li>
                <li>If on-device speech recognition for <var>language</var> is installed, set <var>currentLanguageStatus</var> to {{AvailabilityStatus/available}}.</li>
                <li>Else if on-device speech recognition for <var>language</var> is currently being downloaded, set <var>currentLanguageStatus</var> to {{AvailabilityStatus/downloading}}.</li>
                <li>Else if on-device speech recognition for <var>language</var> is supported by the user agent but not yet installed, set <var>currentLanguageStatus</var> to {{AvailabilityStatus/downloadable}}.</li>
                <li>Else (on-device speech recognition for <var>language</var> is not supported), set <var>currentLanguageStatus</var> to {{AvailabilityStatus/unavailable}}.</li>
                <li>If <var>currentLanguageStatus</var> comes after <var>finalStatus</var> in the ordered list `[{{AvailabilityStatus/available}}, {{AvailabilityStatus/downloading}}, {{AvailabilityStatus/downloadable}}, {{AvailabilityStatus/unavailable}}]`, set <var>finalStatus</var> to <var>currentLanguageStatus</var>.</li>
              </ol>
            </li>
            <li>Let <var>status</var> be <var>finalStatus</var>.</li>
          </ol>
        </li>
      </ol>
1. [=Queue a task=] on the [=relevant global object=]'s [=task queue=] to run the following step:
    - Resolve <var>promise</var> with <var>status</var>.

When the <dfn>start session algorithm</dfn> with
|requestMicrophonePermission| is invoked, the user agent MUST run the
following steps:

1. If the [=current settings object=]'s [=relevant global object=]'s
     [=associated Document=] is NOT [=fully active=], throw an {{InvalidStateError}}
     and abort these steps.
1. If {{SpeechRecognition/[[started]]}} is `true` and no <a event
    for=SpeechRecognition>error</a> event or <a event for=SpeechRecognition>end</a> event
    has fired on it, throw an {{InvalidStateError}} and abort these steps.
1. If this.{{SpeechRecognition/phrases}}'s `length` is greater than 0 and the user agent does not support contextual biasing:
    1. [=Queue a task=] to [=fire an event=] named <a event for=SpeechRecognition>error</a> at [=this=] using {{SpeechRecognitionErrorEvent}} with its {{SpeechRecognitionErrorEvent/error}} attribute initialized to `phrases-not-supported` and its {{SpeechRecognitionErrorEvent/message}} attribute set to an implementation-defined string detailing the reason.
    1. Abort these steps.
1. If this.{{SpeechRecognition/[[processLocally]]}} is `true`:
    1. If the user agent determines that local speech recognition is not available for this.{{SpeechRecognition/lang}}, or if it cannot fulfill the local processing requirement for other reasons:
        1. [=Queue a task=] to [=fire an event=] named <a event for=SpeechRecognition>error</a> at [=this=] using {{SpeechRecognitionErrorEvent}} with its {{SpeechRecognitionErrorEvent/error}} attribute initialized to {{SpeechRecognitionErrorCode/service-not-allowed}} and its {{SpeechRecognitionErrorEvent/message}} attribute set to an implementation-defined string detailing the reason.
        1. Abort these steps.
1. Set {{[[started]]}} to `true`.
1. If |requestMicrophonePermission| is `true` and [=request
    permission to use=] "`microphone`" is [=permission/"denied"=]:
    1. [=Queue a task=] to [=fire an event=] named <a event for=SpeechRecognition>error</a> at [=this=] using {{SpeechRecognitionErrorEvent}} with its {{SpeechRecognitionErrorEvent/error}} attribute initialized to {{SpeechRecognitionErrorCode/not-allowed}} and its {{SpeechRecognitionErrorEvent/message}} attribute set to an implementation-defined string detailing the reason.
    1. Abort these steps.
1. Once the system is successfully listening to the recognition, queue a task to
    [=fire an event=] named <a event for=SpeechRecognition>start</a> at [=this=].

<h4 id="speechreco-events">SpeechRecognition Events</h4>

<p>The DOM Level 2 Event Model is used for speech recognition events.
The methods in the EventTarget interface should be used for registering event listeners.
The SpeechRecognition interface also contains convenience attributes for registering a single event handler for each event type.
These events do not bubble and are not cancelable.</p>

<p>For all these events, the timeStamp attribute defined in the DOM Level 2 Event interface must be set to the best possible estimate of when the real-world event which the event object represents occurred.
This timestamp must be represented in the user agent's view of time, even for events where the timestamps in question could be raised on a different machine like a remote recognition service (i.e., in a <a event for=SpeechRecognition>speechend</a> event with a remote speech endpointer).</p>

<p>Unless specified below, the ordering of the different events is undefined.
For example, some implementations may fire <a event for=SpeechRecognition>audioend</a> before <a event for=SpeechRecognition>speechstart</a> or <a event for=SpeechRecognition>speechend</a> if the audio detector is client-side and the speech detector is server-side.</p>

<dl>
  <dt><dfn event for=SpeechRecognition>audiostart</dfn> event</dt>
  <dd>Fired when the user agent has started to capture audio.</dd>

  <dt><dfn event for=SpeechRecognition>soundstart</dfn> event</dt>
  <dd>Fired when some sound, possibly speech, has been detected.
  This must be fired with low latency, e.g. by using a client-side energy detector.
  The <a event for=SpeechRecognition>audiostart</a> event must always have been fired before the soundstart event.</dd>

  <dt><dfn event for=SpeechRecognition>speechstart</dfn> event</dt>
  <dd>Fired when the speech that will be used for speech recognition has started.
  The <a event for=SpeechRecognition>audiostart</a> event must always have been fired before the speechstart event.</dd>

  <dt><dfn event for=SpeechRecognition>speechend</dfn> event</dt>
  <dd>Fired when the speech that will be used for speech recognition has ended.
  The <a event for=SpeechRecognition>speechstart</a> event must always have been fired before speechend.</dd>

  <dt><dfn event for=SpeechRecognition>soundend</dfn> event</dt>
  <dd>Fired when some sound is no longer detected.
  This must be fired with low latency, e.g. by using a client-side energy detector.
  The <a event for=SpeechRecognition>soundstart</a> event must always have been fired before soundend.</dd>

  <dt><dfn event for=SpeechRecognition>audioend</dfn> event</dt>
  <dd>Fired when the user agent has finished capturing audio.
  The <a event for=SpeechRecognition>audiostart</a> event must always have been fired before audioend.</dd>

  <dt><dfn event for=SpeechRecognition>result</dfn> event</dt>
  <dd>Fired when the speech recognizer returns a result.
  The event must use the {{SpeechRecognitionEvent}} interface.
  The <a event for=SpeechRecognition>audiostart</a> event must always have been fired before the result event.</dd>

  <dt><dfn event for=SpeechRecognition>nomatch</dfn> event</dt>
  <dd>Fired when the speech recognizer returns a final result with no recognition hypothesis that meet or exceed the confidence threshold.
  The event must use the {{SpeechRecognitionEvent}} interface.
  The {{SpeechRecognitionEvent/results}} attribute in the event may contain speech recognition results that are below the confidence threshold or may be null.
  The {{audiostart}} event must always have been fired before the nomatch event.</dd>

  <dt><dfn event for=SpeechRecognition>error</dfn> event</dt>
  <dd>Fired when a speech recognition error occurs.
  The event must use the {{SpeechRecognitionErrorEvent}} interface.</dd>

  <dt><dfn event for=SpeechRecognition>start</dfn> event</dt>
  <dd>Fired when the recognition service has begun to listen to the audio with the intention of recognizing.

  </dd><dt><dfn event for=SpeechRecognition>end</dfn> event</dt>
  <dd>Fired when the service has disconnected.
  The event must always be generated when the session ends no matter the reason for the end.</dd>
</dl>

<h4 id="speechreco-error">SpeechRecognitionErrorEvent</h4>

<p>The {{SpeechRecognitionErrorEvent}} interface is used for the <a event for=SpeechRecognition>error</a> event.</p>
<dl>
  <dt><dfn attribute for=SpeechRecognitionErrorEvent>error</dfn> attribute</dt>
  <dd>The errorCode is an enumeration indicating what has gone wrong.
  The values are:
  <dl>
    <dt><dfn enum-value for=SpeechRecognitionErrorCode>"no-speech"</dfn></dt>
    <dd>No speech was detected.</dd>

    <dt><dfn enum-value for=SpeechRecognitionErrorCode>"aborted"</dfn></dt>
    <dd>Speech input was aborted somehow, maybe by some user-agent-specific behavior such as UI that lets the user cancel speech input.</dd>

    <dt><dfn enum-value for=SpeechRecognitionErrorCode>"audio-capture"</dfn></dt>
    <dd>Audio capture failed.</dd>

    <dt><dfn enum-value for=SpeechRecognitionErrorCode>"network"</dfn></dt>
    <dd>Some network communication that was required to complete the recognition failed.</dd>

    <dt><dfn enum-value for=SpeechRecognitionErrorCode>"not-allowed"</dfn></dt>
    <dd>The user agent is not allowing any speech input to occur for reasons of security, privacy or user preference.</dd>

    <dt><dfn enum-value for=SpeechRecognitionErrorCode>"service-not-allowed"</dfn></dt>
    <dd>The user agent is not allowing the web application requested speech service, but would allow some speech service, to be used either because the user agent doesn't support the selected one or because of reasons of security, privacy or user preference.</dd>

    <dt><dfn enum-value for=SpeechRecognitionErrorCode>"language-not-supported"</dfn></dt>
    <dd>The language was not supported.</dd>

    <dt><dfn enum-value for=SpeechRecognitionErrorCode>"phrases-not-supported"</dfn></dt>
    <dd>The speech recognition model does not support phrases for contextual biasing.</dd>
  </dl>
  </dd>

  <dt><dfn attribute for=SpeechRecognitionErrorEvent>message</dfn> attribute</dt>
  <dd>The message content is implementation specific.
  This attribute is primarily intended for debugging and developers should not use it directly in their application user interface.</dd>
</dl>

<h4 id="speechreco-alternative">SpeechRecognitionAlternative</h4>

<p>The SpeechRecognitionAlternative represents a simple view of the response that gets used in a n-best list.

<dl>
  <dt><dfn attribute for=SpeechRecognitionAlternative>transcript</dfn> attribute</dt>
  <dd>The transcript string represents the raw words that the user spoke.
  For continuous recognition, leading or trailing whitespace MUST be included where necessary such that concatenation of consecutive SpeechRecognitionResults produces a proper transcript of the session.</dd>

  <dt><dfn attribute for=SpeechRecognitionAlternative>confidence</dfn> attribute</dt>
  <dd>The confidence represents a numeric estimate between 0 and 1 of how confident the recognition system is that the recognition is correct.
  A higher number means the system is more confident.
  <p class=issue>The group has discussed whether confidence can be specified in a speech-recognition-engine-independent manner and whether confidence threshold and nomatch should be included, because this is not a dialog API.
  See <a href="https://lists.w3.org/Archives/Public/public-speech-api/2012Jun/0143.html">Confidence property</a> thread on public-speech-api@w3.org.</p></dd>
</dl>

<h4 id="speechreco-result">SpeechRecognitionResult</h4>

<p>The SpeechRecognitionResult object represents a single one-shot recognition match, either as one small part of a continuous recognition or as the complete return result of a non-continuous recognition.</p>

<dl>
  <dt><dfn attribute for=SpeechRecognitionResult>length</dfn> attribute</dt>
  <dd>The long attribute represents how many n-best alternatives are represented in the item array.</dd>

  <dt><dfn method for=SpeechRecognitionResult>item(<var>index</var>)</dfn> getter</dt>
  <dd>The item getter returns a SpeechRecognitionAlternative from the index into an array of n-best values.
  If index is greater than or equal to length, this returns null.
  The user agent must ensure that the length attribute is set to the number of elements in the array.
  The user agent must ensure that the n-best list is sorted in non-increasing confidence order (each element must be less than or equal to the confidence of the preceding elements).</dd>

  <dt><dfn attribute for=SpeechRecognitionResult>isFinal</dfn> attribute</dt>
  <dd>The final boolean must be set to true if this is the final time the speech service will return this particular index value.
  If the value is false, then this represents an interim result that could still be changed.</dd>
</dl>

<h4 id="speechreco-resultlist">SpeechRecognitionResultList</h4>

<p>The SpeechRecognitionResultList object holds a sequence of recognition results representing the complete return result of a continuous recognition.
For a non-continuous recognition it will hold only a single value.</p>

<dl>
  <dt><dfn attribute for=SpeechRecognitionResultList>length</dfn> attribute</dt>
  <dd>The length attribute indicates how many results are represented in the item array.</dd>

  <dt><dfn method for=SpeechRecognitionResultList>item(<var>index</var>)</dfn> getter</dt>
  <dd>The item getter returns a SpeechRecognitionResult from the index into an array of result values.
  If index is greater than or equal to length, this returns null.
  The user agent must ensure that the length attribute is set to the number of elements in the array.</dd>
</dl>

<h4 id="speechreco-event">SpeechRecognitionEvent</h4>

<p>The SpeechRecognitionEvent is the event that is raised each time there are any changes to interim or final results.</p>

<dl>
  <dt><dfn attribute for=SpeechRecognitionEvent>resultIndex</dfn> attribute</dt>
  <dd>The resultIndex must be set to the lowest index in the "results" array that has changed.</dd>

  <dt><dfn attribute for=SpeechRecognitionEvent>results</dfn> attribute</dt>
  <dd>The array of all current recognition results for this session.
  Specifically all final results that have been returned, followed by the current best hypothesis for all interim results.
  It must consist of zero or more final results followed by zero or more interim results.
  On subsequent SpeechRecognitionResultEvent events, interim results may be overwritten by a newer interim result or by a final result or may be removed (when at the end of the "results" array and the array length decreases).
  Final results must not be overwritten or removed.
  All entries for indexes less than resultIndex must be identical to the array that was present when the last SpeechRecognitionResultEvent was raised.
  All array entries (if any) for indexes equal or greater than resultIndex that were present in the array when the last SpeechRecognitionResultEvent was raised are removed and overwritten with new results.
  The length of the "results" array may increase or decrease, but must not be less than resultIndex.
  Note that when resultIndex equals results.length, no new results are returned, this may occur when the array length decreases to remove one or more interim results.</dd>
</dl>

<h4 id="speechreco-phrase">SpeechRecognitionPhrase</h4>

<p>The SpeechRecognitionPhrase object represents a phrase for contextual biasing and has the following internal slots:</p>

<dl dfn-type=attribute dfn-for="SpeechRecognitionPhrase">
    : <dfn>[[phrase]]</dfn>
    ::
        A {{DOMString}} representing the text string to be boosted. The initial value is null.
        An empty value is allowed but should be ignored by the speech recognition model.
</dl>

<dl dfn-type=attribute dfn-for="SpeechRecognitionPhrase">
    : <dfn>[[boost]]</dfn>
    ::
        A float representing approximately the natural log of the number of times more likely the website thinks this phrase is
        than what the speech recognition model knows.
        A valid boost must be a float value inside the range [0.0, 10.0], with a default value of 1.0 if not specified.
        A boost of 0.0 means the phrase is not boosted at all, and a higher boost means the phrase is more likely to appear.
        A boost of 10.0 means the phrase is extremely likely to appear and should be rarely set.
</dl>

<dl>
  <dt><dfn constructor for=SpeechRecognitionPhrase>SpeechRecognitionPhrase(|phrase|, |boost|)</dfn> constructor</dt>
  <dd>
    When this constructor is invoked, run the following steps:
    1. If |boost| is smaller than 0.0 or greater than 10.0, throw a {{SyntaxError}} and abort these steps.
    1. Let |phr| be a new object of type {{SpeechRecognitionPhrase}}.
    1. Set |phr|.{{[[phrase]]}} to be the value of |phrase|.
    1. Set |phr|.{{[[boost]]}} to be the value of |boost|.
    1. Return |phr|.
  </dd>

  <dt><dfn attribute for=SpeechRecognitionPhrase>phrase</dfn> attribute</dt>
  <dd>This attribute returns the value of {{[[phrase]]}}.</dd>

  <dt><dfn attribute for=SpeechRecognitionPhrase>boost</dfn> attribute</dt>
  <dd>This attribute returns the value of {{[[boost]]}}.</dd>
</dl>

<h4 id="speechreco-speechgrammar">SpeechGrammar</h4>

<p>The SpeechGrammar object represents a container for a grammar.</p>
<p class=note>Grammar support has been deprecated and removed. The grammar objects remain in the spec for backwards compatibility purposes only and do not affect speech recognition.</p>
<p>This structure has the following attributes:</p>

<dl>
  <dt><dfn attribute for=SpeechGrammar>src</dfn> attribute</dt>
  <dd>The required src attribute is the URI for the grammar.</dd>

  <dt><dfn attribute for=SpeechGrammar>weight</dfn> attribute</dt>
  <dd>The optional weight attribute controls the weight that the speech recognition service should use with this grammar.
  By default, a grammar has a weight of 1.
  Larger weight values positively weight the grammar while smaller weight values make the grammar weighted less strongly.</dd>
</dl>

<h4 id="speechreco-speechgrammarlist">SpeechGrammarList</h4>

<p>The SpeechGrammarList object represents a collection of SpeechGrammar objects.
This structure has the following attributes:</p>
<p class=note>Grammar support has been deprecated and removed. The grammar objects remain in the spec for backwards compatibility purposes only and do not affect speech recognition.</p>

<dl>
  <dt><dfn attribute for=SpeechGrammarList>length</dfn> attribute</dt>
  <dd>The length attribute represents how many grammars are currently in the array.</dd>

  <dt><dfn method for=SpeechGrammarList>item(<var>index</var>)</dfn> getter</dt>
  <dd>The item getter returns a SpeechGrammar from the index into an array of grammars.
  The user agent must ensure that the length attribute is set to the number of elements in the array.
  The user agent must ensure that the index order from smallest to largest matches the order in which grammars were added to the array.</dd>

  <dt><dfn method for=SpeechGrammarList>addFromURI(<var>src</var>, <var>weight</var>)</dfn> method</dt>
  <dd>This method appends a grammar to the grammars array parameter based on URI.
  The URI for the grammar is specified by the <var>src</var> parameter, which represents the URI for the grammar.
  Note, some services may support builtin grammars that can be specified by URI.
  The <var>weight</var> parameter represents this grammar's weight relative to the other grammar.

  <dt><dfn method for=SpeechGrammarList>addFromString(<var>string</var>, <var>weight</var>)</dfn> method</dt>
  <dd>This method appends a grammar to the grammars array parameter based on text.
  The content of the grammar is specified by the <var>string</var> parameter.
  This content should be encoded into a data: URI when the SpeechGrammar object is created.
  The <var>weight</var> parameter represents this grammar's weight relative to the other grammar.
</dl>
