/* this source code form is subject to the terms of the mozilla public
 * license, v. 2.0. if a copy of the mpl was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_AUDIONODEENGINEGENERIC_H_
#define MOZILLA_AUDIONODEENGINEGENERIC_H_

#include "AudioNodeEngine.h"
#include "xsimd/xsimd.hpp"

namespace mozilla {

template <class Arch>
struct Engine {
  static void AudioBufferAddWithScale(const float* aInput, float aScale,
                                      float* aOutput, uint32_t aSize);

  static void AudioBufferCopyChannelWithScale(const float* aInput, float aScale,
                                             float* aOutput, uint32_t aSize);

  static void AudioBufferCopyChannelWithScale(const float* aInput,
                                             const float* aScale,
                                             float* aOutput, uint32_t aSize);

  static void AudioBufferInPlaceScale(float* aBlock, float aScale,
                                      uint32_t aSize);

  static void AudioBufferInPlaceScale(float* aBlock, float* aScale,
                                      uint32_t aSize);

  static void AudioBufferPanStereoToStereo(const float* aInputL,
                                          const float* aInputR, float aGainL,
                                          float aGainR, bool aIsOnTheLeft,
                                          float* aOutputL, float* aOutputR,
                                          uint32_t aSize);

  static void BufferComplexMultiply(const float* aInput, const float* aScale,
                                    float* aOutput, uint32_t aSize);

  static float AudioBufferSumOfSquares(const float* aInput, uint32_t aLength);

  static void NaNToZeroInPlace(float* aSamples, size_t aCount);
