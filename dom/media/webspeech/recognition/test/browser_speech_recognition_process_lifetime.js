/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// The HWInference utility process must not outlive the speech recognition that
// needed it: once the last session ends and the last availability/install call
// settles (and, after the grace period, nobody has come back), it is shut down
// rather than lingering until browser shutdown.

const ORIGIN = "https://example.com";
const PAGE =
  getRootDirectory(gTestPath).replace("chrome://mochitests/content", ORIGIN) +
  "empty.html";

async function hwInferenceProcessCount() {
  const info = await ChromeUtils.requestProcInfo();
  return info.children.filter(
    child =>
      child.type == "utility" &&
      child.utilityActors.some(actor => actor.actorName == "hwInference")
  ).length;
}

function waitForHWInferenceProcessCount(expected, msg) {
  return TestUtils.waitForCondition(
    async () => (await hwInferenceProcessCount()) == expected,
    msg
  );
}

function callAvailable(browser) {
  return SpecialPowers.spawn(browser, [], () =>
    content.SpeechRecognition.available({
      langs: ["en-US"],
      processLocally: true,
    })
  );
}

add_setup(async function () {
  await SpecialPowers.pushPrefEnv({
    set: [
      ["media.webspeech.recognition.enable", true],
      // No network/IndexedDB/downloads, and RecvInit skips model retrieval, so
      // a session can start without a real multi-hundred-MB model.
      ["browser.ml.modelHub.testing", true],
      // Fake mic, so start() gets a track without any device or user prompt.
      ["media.navigator.streams.fake", true],
      ["media.navigator.permission.disabled", true],
    ],
  });
});

add_task(async function test_session_then_tab_close_shuts_process_down() {
  await SpecialPowers.pushPrefEnv({
    set: [["media.webspeech.recognition.idle_shutdown_grace_ms", 0]],
  });

  const before = await hwInferenceProcessCount();
  is(before, 0, "No HWInference process before the test");

  const tab = await BrowserTestUtils.openNewForegroundTab(gBrowser, PAGE);

  const started = await SpecialPowers.spawn(tab.linkedBrowser, [], async () => {
    const stream = await content.navigator.mediaDevices.getUserMedia({
      audio: true,
    });
    const recognition = new content.SpeechRecognition();
    recognition.processLocally = true;
    recognition.lang = "en-US";
    // Keep it reachable so it stays alive until the tab goes away, which is
    // what this test is about.
    content.wrappedJSObject._recognition = recognition;
    return new Promise(resolve => {
      recognition.onstart = () => resolve("start");
      recognition.onerror = e => resolve(`error: ${e.error}`);
      recognition.start(stream.getAudioTracks()[0]);
    });
  });
  is(started, "start", "Recognition session started");

  await waitForHWInferenceProcessCount(
    1,
    "HWInference process is running while the session is active"
  );

  BrowserTestUtils.removeTab(tab);

  await waitForHWInferenceProcessCount(
    0,
    "HWInference process is shut down once the tab owning the session is gone"
  );

  await SpecialPowers.popPrefEnv();
});

// A live SpeechRecognition object holds the process even without a session,
// so start() doesn't have to wait for a relaunch.
add_task(async function test_live_object_holds_process() {
  await SpecialPowers.pushPrefEnv({
    set: [["media.webspeech.recognition.idle_shutdown_grace_ms", 0]],
  });

  await BrowserTestUtils.withNewTab(PAGE, async browser => {
    // available() launches the process, and settles, but the object created
    // here keeps it up afterwards.
    await SpecialPowers.spawn(browser, [], () => {
      content.wrappedJSObject._recognition = new content.SpeechRecognition();
    });
    await callAvailable(browser);

    await waitForHWInferenceProcessCount(
      1,
      "The live SpeechRecognition object keeps the process up after " +
        "available() settled"
    );

    // Dropping the last reference releases it.
    await SpecialPowers.spawn(browser, [], async () => {
      content.wrappedJSObject._recognition = null;
      SpecialPowers.Cu.forceGC();
      SpecialPowers.Cu.forceCC();
    });

    await waitForHWInferenceProcessCount(
      0,
      "Process is shut down once the last SpeechRecognition object is gone"
    );
  });

  await SpecialPowers.popPrefEnv();
});

// A one-shot static call is a "transaction": it holds the process for its
// duration and releases it when it settles.
add_task(async function test_transaction_releases_process() {
  await SpecialPowers.pushPrefEnv({
    set: [["media.webspeech.recognition.idle_shutdown_grace_ms", 0]],
  });

  await BrowserTestUtils.withNewTab(PAGE, async browser => {
    await callAvailable(browser);

    await waitForHWInferenceProcessCount(
      0,
      "HWInference process is shut down once available() has settled"
    );
  });

  await SpecialPowers.popPrefEnv();
});

// With a grace period, the process is kept warm briefly so a stop()/start()
// cycle reuses it instead of paying for a relaunch -- but it still goes away
// on its own once the grace period elapses.
add_task(async function test_grace_period_keeps_process_warm() {
  await SpecialPowers.pushPrefEnv({
    set: [["media.webspeech.recognition.idle_shutdown_grace_ms", 5000]],
  });

  await BrowserTestUtils.withNewTab(PAGE, async browser => {
    await callAvailable(browser);

    await waitForHWInferenceProcessCount(
      1,
      "HWInference process launched by available()"
    );

    // Still up right after the call settled: the shutdown is only pending.
    is(
      await hwInferenceProcessCount(),
      1,
      "HWInference process is kept warm during the grace period"
    );

    // A second call within the grace period reuses the warm process.
    await callAvailable(browser);
    is(
      await hwInferenceProcessCount(),
      1,
      "The warm HWInference process is reused rather than relaunched"
    );

    // Shorten the grace period so the pending close lands promptly.
    await SpecialPowers.pushPrefEnv({
      set: [["media.webspeech.recognition.idle_shutdown_grace_ms", 0]],
    });
    await callAvailable(browser);

    await waitForHWInferenceProcessCount(
      0,
      "HWInference process is shut down once the grace period elapses"
    );
    await SpecialPowers.popPrefEnv();
  });

  await SpecialPowers.popPrefEnv();
});
