/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FFTBlock.h"

#include <cmath>

#include "AlignedTArray.h"
#include "mozilla/gfx/gfxVars.h"
#include "gtest/gtest.h"

using namespace mozilla;

// Round-trip a pure tone through forward + inverse FFT and verify reconstruction.
// This works for any even FFT size, including non-power-of-2.
static void TestRoundTrip(uint32_t aFFTSize) {
  gfx::gfxVars::Initialize();
  FFTBlock::MainThreadInit();

  // Generate a sine wave at frequency bin 1 (one full cycle across the buffer)
  AlignedTArray<float> input(aFFTSize);
  for (uint32_t i = 0; i < aFFTSize; i++) {
    input[i] = sinf(2.0f * float(M_PI) * float(i) / float(aFFTSize));
  }

  float scale = 1.0f / float(aFFTSize);
  FFTBlock block(aFFTSize, scale);

  block.PerformFFT(input.Elements());

  AlignedTArray<float> output(aFFTSize);
  block.GetInverse(output.Elements());

  // Check reconstruction within floating-point tolerance
  float maxErr = 0.0f;
  for (uint32_t i = 0; i < aFFTSize; i++) {
    maxErr = std::max(maxErr, std::abs(output[i] - input[i]));
  }
  EXPECT_LT(maxErr, 1e-4f) << "Round-trip error too large for fftSize=" << aFFTSize;
}

TEST(FFTBlock, RoundTripPowerOfTwo)
{
  for (uint32_t size : {128u, 256u, 512u, 1024u}) {
    TestRoundTrip(size);
  }
}

TEST(FFTBlock, RoundTripNonPowerOfTwo)
{
  // Typical non-power-of-2 sizes arising from non-standard render quanta
  // and their OLA-doubled FFT sizes:
  //   96 frames  @ 48kHz (some Android devices)
  //   192 frames @ 48kHz (common Android)
  //   240 frames @ 44.1kHz (some Android)
  //   440 frames (WASAPI typical at 44.1kHz)
  //   480 frames (WASAPI typical at 48kHz)
  for (uint32_t size : {96u, 192u, 240u, 384u, 440u, 480u,
                        768u, 880u, 960u, 1152u, 1760u}) {
    TestRoundTrip(size);
  }
}
