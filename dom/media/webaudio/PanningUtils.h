/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef PANNING_UTILS_H
#define PANNING_UTILS_H

#include "AudioNodeEngine.h"
#include "AudioSegment.h"

namespace mozilla::dom {

template <typename T>
void GainMonoToStereo(const AudioBlock& aInput, AudioBlock* aOutput, T aGainL,
                      T aGainR, uint32_t aSize) {
  float* outputL = aOutput->ChannelFloatsForWrite(0);
  float* outputR = aOutput->ChannelFloatsForWrite(1);
  const float* input = static_cast<const float*>(aInput.mChannelData[0]);

  MOZ_ASSERT(aInput.ChannelCount() == 1);
  MOZ_ASSERT(aOutput->ChannelCount() == 2);

  AudioBufferPanMonoToStereo(input, aGainL, aGainR, outputL, outputR, aSize);
}

// T can be float or an array of float, and  U can be bool or an array of bool,
// depending if the value of the parameters are constant for this block.
template <typename T, typename U>
void GainStereoToStereo(const AudioBlock& aInput, AudioBlock* aOutput, T aGainL,
                        T aGainR, U aOnLeft, uint32_t aSize) {
  float* outputL = aOutput->ChannelFloatsForWrite(0);
  float* outputR = aOutput->ChannelFloatsForWrite(1);
  const float* inputL = static_cast<const float*>(aInput.mChannelData[0]);
  const float* inputR = static_cast<const float*>(aInput.mChannelData[1]);

  MOZ_ASSERT(aInput.ChannelCount() == 2);
  MOZ_ASSERT(aOutput->ChannelCount() == 2);

  AudioBufferPanStereoToStereo(inputL, inputR, aGainL, aGainR, aOnLeft, outputL,
                              outputR, aSize);
}

// T can be float or an array of float, and  U can be bool or an array of bool,
// depending if the value of the parameters are constant for this block.
template <typename T, typename U>
void ApplyStereoPanning(const AudioBlock& aInput, AudioBlock* aOutput, T aGainL,
                        T aGainR, U aOnLeft, uint32_t aSize) {
  aOutput->AllocateChannels(2);

  if (aInput.ChannelCount() == 1) {
    GainMonoToStereo(aInput, aOutput, aGainL, aGainR, aSize);
  } else {
    GainStereoToStereo(aInput, aOutput, aGainL, aGainR, aOnLeft, aSize);
  }
  aOutput->mVolume = aInput.mVolume;
}

}  // namespace mozilla::dom

#endif  // PANNING_UTILS_H
