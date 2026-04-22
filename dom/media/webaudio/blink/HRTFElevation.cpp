/*
 * Copyright (C) 2010 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "HRTFElevation.h"

#include <speex/speex_resampler.h>

#include "AudioSampleFormat.h"
#include "IRC_Composite_C_R0195-incl.cpp"
#include "mozilla/PodOperations.h"

using namespace mozilla;

namespace WebCore {

const int elevationSpacing = irc_composite_c_r0195_elevation_interval;
const int firstElevation = irc_composite_c_r0195_first_elevation;
const int numberOfElevations = std::size(irc_composite_c_r0195);

const unsigned HRTFElevation::NumberOfTotalAzimuths = 360 / 15 * 8;

const int rawSampleRate = irc_composite_c_r0195_sample_rate;

// Number of frames in an individual impulse response.
const size_t ResponseFrameSize = 256;

size_t HRTFElevation::sizeOfIncludingThis(
    mozilla::MallocSizeOf aMallocSizeOf) const {
  size_t amount = aMallocSizeOf(this);

  amount += m_kernelListL.ShallowSizeOfExcludingThis(aMallocSizeOf);
  for (size_t i = 0; i < m_kernelListL.Length(); i++) {
    amount += m_kernelListL[i]->sizeOfIncludingThis(aMallocSizeOf);
  }

  return amount;
}

size_t HRTFElevation::fftSizeForSampleRate(float sampleRate, size_t blockSize) {
  // The IRCAM HRTF impulse responses were 512 sample-frames @44.1KHz,
  // but these have been truncated to 256 samples.
  // An FFT-size of twice impulse response size is used (for convolution).
  // If the FFT size is too large then the impulse response will be padded
  // with zeros without the fade-out provided by HRTFKernel.
  MOZ_ASSERT(sampleRate > 1.0 && sampleRate < 1048576.0);

  unsigned resampledLength =
      floorf(ResponseFrameSize * sampleRate / rawSampleRate);
  // Cap at 512 samples — same effective halfSize limit as the old 1024-cap
  // after power-of-2 rounding.
  resampledLength = std::min(resampledLength, 512U);

  // Smallest fftSize that is a multiple of 2*blockSize and >= 2*resampledLength.
  // This ensures halfSize = fftSize/2 satisfies:
  //   halfSize % blockSize == 0  (FFTConvolver OLA invariant, any quantum size)
  //   halfSize >= resampledLength  (sufficient zero-padding for linear convolution)
  size_t k = (resampledLength + blockSize - 1) / blockSize;
  return k * 2 * blockSize;
}

nsReturnRef<HRTFKernel> HRTFElevation::calculateKernelForAzimuthElevation(
    int azimuth, int elevation, SpeexResamplerState* resampler,
    float sampleRate) {
  int elevationIndex = (elevation - firstElevation) / elevationSpacing;
  MOZ_ASSERT(elevationIndex >= 0 && elevationIndex <= numberOfElevations);

  int numberOfAzimuths = irc_composite_c_r0195[elevationIndex].count;
  int azimuthSpacing = 360 / numberOfAzimuths;
  MOZ_ASSERT(numberOfAzimuths * azimuthSpacing == 360);

  int azimuthIndex = azimuth / azimuthSpacing;
  MOZ_ASSERT(azimuthIndex * azimuthSpacing == azimuth);

  const int16_t (&impulse_response_data)[ResponseFrameSize] =
      irc_composite_c_r0195[elevationIndex].azimuths[azimuthIndex];

  float response[ResponseFrameSize];
  ConvertAudioSamples(impulse_response_data, response, ResponseFrameSize);
  float* resampledResponse;

  // Note that depending on the fftSize returned by the panner, we may be
  // truncating the impulse response.
  const size_t resampledResponseLength =
      fftSizeForSampleRate(sampleRate, WEBAUDIO_BLOCK_SIZE) / 2;

  AutoTArray<AudioDataValue, 2 * ResponseFrameSize> resampled;
  if (sampleRate == rawSampleRate) {
    resampledResponse = response;
    MOZ_ASSERT(resampledResponseLength == ResponseFrameSize);
  } else {
    resampled.SetLength(resampledResponseLength);
    resampledResponse = resampled.Elements();
    speex_resampler_skip_zeros(resampler);

    // Feed the input buffer into the resampler.
    spx_uint32_t in_len = ResponseFrameSize;
    spx_uint32_t out_len = resampled.Length();
    speex_resampler_process_float(resampler, 0, response, &in_len,
                                  resampled.Elements(), &out_len);

    if (out_len < resampled.Length()) {
      // The input should have all been processed.
      MOZ_ASSERT(in_len == ResponseFrameSize);
      // Feed in zeros get the data remaining in the resampler.
      spx_uint32_t out_index = out_len;
      in_len = speex_resampler_get_input_latency(resampler);
      out_len = resampled.Length() - out_index;
      speex_resampler_process_float(resampler, 0, nullptr, &in_len,
                                    resampled.Elements() + out_index, &out_len);
      out_index += out_len;
      // There may be some uninitialized samples remaining for very low
      // sample rates.
      PodZero(resampled.Elements() + out_index, resampled.Length() - out_index);
    }

    speex_resampler_reset_mem(resampler);
  }

  return HRTFKernel::create(resampledResponse, resampledResponseLength,
                            sampleRate);
}

// The range of elevations for the IRCAM impulse responses varies depending on
// azimuth, but the minimum elevation appears to always be -45.
//
// Here's how it goes:
static int maxElevations[] = {
    //  Azimuth
    //
    90,  // 0
    45,  // 15
    60,  // 30
    45,  // 45
    75,  // 60
    45,  // 75
    60,  // 90
    45,  // 105
    75,  // 120
    45,  // 135
    60,  // 150
    45,  // 165
    75,  // 180
    45,  // 195
    60,  // 210
    45,  // 225
    75,  // 240
    45,  // 255
    60,  // 270
    45,  // 285
    75,  // 300
    45,  // 315
    60,  // 330
    45   //  345
};

nsReturnRef<HRTFElevation> HRTFElevation::createBuiltin(int elevation,
                                                        float sampleRate) {
  if (elevation < firstElevation ||
      elevation > firstElevation + numberOfElevations * elevationSpacing ||
      (elevation / elevationSpacing) * elevationSpacing != elevation)
    return nsReturnRef<HRTFElevation>();

  // Spacing, in degrees, between every azimuth loaded from resource.
  // Some elevations do not have data for all these intervals.
  // See maxElevations.
  static const unsigned AzimuthSpacing = 15;
  static const unsigned NumberOfRawAzimuths = 360 / AzimuthSpacing;
  static_assert(AzimuthSpacing * NumberOfRawAzimuths == 360, "Not a multiple");
  static const unsigned InterpolationFactor =
      NumberOfTotalAzimuths / NumberOfRawAzimuths;
  static_assert(
      NumberOfTotalAzimuths == NumberOfRawAzimuths * InterpolationFactor,
      "Not a multiple");

  HRTFKernelList kernelListL;
  kernelListL.SetLength(NumberOfTotalAzimuths);

  SpeexResamplerState* resampler =
      sampleRate == rawSampleRate
          ? nullptr
          : speex_resampler_init(1, rawSampleRate, sampleRate,
                                 SPEEX_RESAMPLER_QUALITY_MIN, nullptr);

  // Load convolution kernels from HRTF files.
  int interpolatedIndex = 0;
  for (unsigned rawIndex = 0; rawIndex < NumberOfRawAzimuths; ++rawIndex) {
    // Don't let elevation exceed maximum for this azimuth.
    int maxElevation = maxElevations[rawIndex];
    int actualElevation = std::min(elevation, maxElevation);

    kernelListL[interpolatedIndex] = calculateKernelForAzimuthElevation(
        rawIndex * AzimuthSpacing, actualElevation, resampler, sampleRate);

    interpolatedIndex += InterpolationFactor;
  }

  if (resampler) speex_resampler_destroy(resampler);

  // Now go back and interpolate intermediate azimuth values.
  for (unsigned i = 0; i < NumberOfTotalAzimuths; i += InterpolationFactor) {
    int j = (i + InterpolationFactor) % NumberOfTotalAzimuths;

    // Create the interpolated convolution kernels and delays.
    for (unsigned jj = 1; jj < InterpolationFactor; ++jj) {
      float x =
          float(jj) / float(InterpolationFactor);  // interpolate from 0 -> 1

      kernelListL[i + jj] = HRTFKernel::createInterpolatedKernel(
          kernelListL[i], kernelListL[j], x);
    }
  }

  return nsReturnRef<HRTFElevation>(
      new HRTFElevation(std::move(kernelListL), elevation, sampleRate));
}

nsReturnRef<HRTFElevation> HRTFElevation::createByInterpolatingSlices(
    HRTFElevation* hrtfElevation1, HRTFElevation* hrtfElevation2, float x,
    float sampleRate) {
  MOZ_ASSERT(hrtfElevation1 && hrtfElevation2);
  if (!hrtfElevation1 || !hrtfElevation2) return nsReturnRef<HRTFElevation>();

  MOZ_ASSERT(x >= 0.0 && x < 1.0);

  HRTFKernelList kernelListL;
  kernelListL.SetLength(NumberOfTotalAzimuths);

  const HRTFKernelList& kernelListL1 = hrtfElevation1->kernelListL();
  const HRTFKernelList& kernelListL2 = hrtfElevation2->kernelListL();

  // Interpolate kernels of corresponding azimuths of the two elevations.
  for (unsigned i = 0; i < NumberOfTotalAzimuths; ++i) {
    kernelListL[i] = HRTFKernel::createInterpolatedKernel(kernelListL1[i],
                                                          kernelListL2[i], x);
  }

  // Interpolate elevation angle.
  double angle = (1.0 - x) * hrtfElevation1->elevationAngle() +
                 x * hrtfElevation2->elevationAngle();

  return nsReturnRef<HRTFElevation>(new HRTFElevation(
      std::move(kernelListL), static_cast<int>(angle), sampleRate));
}

void HRTFElevation::getKernelsFromAzimuth(
    double azimuthBlend, unsigned azimuthIndex, HRTFKernel*& kernelL,
    HRTFKernel*& kernelR, double& frameDelayL, double& frameDelayR) {
  bool checkAzimuthBlend = azimuthBlend >= 0.0 && azimuthBlend < 1.0;
  MOZ_ASSERT(checkAzimuthBlend);
  if (!checkAzimuthBlend) azimuthBlend = 0.0;

  unsigned numKernels = m_kernelListL.Length();

  bool isIndexGood = azimuthIndex < numKernels;
  MOZ_ASSERT(isIndexGood);
  if (!isIndexGood) {
    kernelL = 0;
    kernelR = 0;
    return;
  }

  // Return the left and right kernels,
  // using symmetry to produce the right kernel.
  kernelL = m_kernelListL[azimuthIndex];
  int azimuthIndexR = (numKernels - azimuthIndex) % numKernels;
  kernelR = m_kernelListL[azimuthIndexR];

  frameDelayL = kernelL->frameDelay();
  frameDelayR = kernelR->frameDelay();

  int azimuthIndex2L = (azimuthIndex + 1) % numKernels;
  double frameDelay2L = m_kernelListL[azimuthIndex2L]->frameDelay();
  int azimuthIndex2R = (numKernels - azimuthIndex2L) % numKernels;
  double frameDelay2R = m_kernelListL[azimuthIndex2R]->frameDelay();

  // Linearly interpolate delays.
  frameDelayL =
      (1.0 - azimuthBlend) * frameDelayL + azimuthBlend * frameDelay2L;
  frameDelayR =
      (1.0 - azimuthBlend) * frameDelayR + azimuthBlend * frameDelay2R;
}

}  // namespace WebCore
