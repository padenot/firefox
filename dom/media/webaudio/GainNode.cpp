/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GainNode.h"

#include "AlignmentUtils.h"
#include "AudioDestinationNode.h"
#include "AudioNodeEngine.h"
#include "AudioNodeTrack.h"
#include "Tracing.h"
#include "WebAudioUtils.h"
#include "mozilla/dom/GainNodeBinding.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION_INHERITED(GainNode, AudioNode, mGain)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(GainNode)
NS_INTERFACE_MAP_END_INHERITING(AudioNode)

NS_IMPL_ADDREF_INHERITED(GainNode, AudioNode)
NS_IMPL_RELEASE_INHERITED(GainNode, AudioNode)

class GainNodeEngine final : public AudioNodeEngine {
 public:
  GainNodeEngine(AudioNode* aNode, AudioDestinationNode* aDestination)
      : AudioNodeEngine(aNode),
        mDestination(aDestination->Track())
        // Keep the default value in sync with the default value in
        // GainNode::GainNode.
        ,
        mGain(1.f) {}

  enum Parameters { GAIN };
  void RecvTimelineEvent(uint32_t aIndex, AudioParamEvent& aEvent) override {
    MOZ_ASSERT(mDestination);
    aEvent.ConvertToTicks(mDestination);

    switch (aIndex) {
      case GAIN:
        mGain.InsertEvent<int64_t>(aEvent);
        break;
      default:
        NS_ERROR("Bad GainNodeEngine TimelineParameter");
    }
  }

  void ProcessBlock(AudioNodeTrack* aTrack, GraphTime aFrom,
                    const AudioBlock& aInput, AudioBlock* aOutput,
                    bool* aFinished) override {
    TRACE("GainNodeEngine::ProcessBlock");
    if (aInput.IsNull()) {
      // If input is silent, so is the output
      aOutput->SetNull(aTrack->BlockSize());
    } else if (mGain.HasSimpleValue()) {
      // Optimize the case where we only have a single value set as the volume
      float gain = mGain.GetValue();
      if (gain == 0.0f) {
        aOutput->SetNull(aTrack->BlockSize());
      } else {
        *aOutput = aInput;
        aOutput->mVolume *= gain;
      }
    } else {
      // First, compute a vector of gains for each track tick based on the
      // timeline at hand, and then for each channel, multiply the values
      // in the buffer with the gain vector.
      aOutput->AllocateChannels(aInput.ChannelCount(), aTrack->BlockSize());

      // Compute the gain values for the duration of the input AudioChunk
      TrackTime tick = mDestination->GraphTimeToTrackTime(aFrom);
      auto alignedComputedGain = aTrack->GetScratch<float>(aTrack->BlockSize());
      ASSERT_ALIGNED16(alignedComputedGain.data());
      mGain.GetValuesAtTime(tick, alignedComputedGain.data(), aTrack->BlockSize(),
                            aTrack->BlockSize());

      for (size_t counter = 0; counter < aTrack->BlockSize(); ++counter) {
        alignedComputedGain[counter] *= aInput.mVolume;
      }

      // Apply the gain to the output buffer
      for (size_t channel = 0; channel < aOutput->ChannelCount(); ++channel) {
        const float* inputBuffer =
            static_cast<const float*>(aInput.mChannelData[channel]);