/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

add_task(async function test_browser_hwinference_smoke_test() {
  // The smoke test drives the model-provisioning path (resolver + download
  // gate) via InstallModel before computing. Use the ModelHub testing mock so
  // the install is satisfied locally instead of hitting the network.
  await SpecialPowers.pushPrefEnv({
    set: [["browser.ml.modelHub.testing", true]],
  });

  const result = await Cc["@mozilla.org/ml-utils;1"]
    .getService(Ci.nsIMLUtils)
    .runHWInferenceSmokeTest();

  Assert.equal(result, 10, "The browser HWInference smoke test should run");
});
