/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "BiquadFilterNode.h"

#include <algorithm>

#include "AlignmentUtils.h"
#include "AudioDestinationNode.h"
#include "AudioNodeEngine.h"
#include "AudioNodeTrack.h"
#include "AudioParamTimeline.h"
#include "PlayingRefChangeHandler.h"
#include "Tracing.h"
#include "WebAudioUtils.h"
#include "blink/Biquad.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/UniquePtr.h"
#include "nsGlobalWindowInner.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION_INHERITED(BiquadFilterNode, AudioNode, mFrequency,
                                   mDetune, mQ, mGain)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(BiquadFilterNode)
NS_INTERFACE_MAP_END_INHERITING(AudioNode)

NS_IMPL_ADDREF_INHERITED(BiquadFilterNode, AudioNode)
NS_IMPL_RELEASE_INHERITED(BiquadFilterNode, AudioNode)

static void SetParamsOnBiquad(WebCore::Biquad& aBiquad, float aSampleRate,
                              BiquadFilterType aType, double aFrequency,
                              double aQ, double aGain, double aDetune) {
  const double nyquist = aSampleRate * 0.5;
  double normalizedFrequency = aFrequency / nyquist;

  if (aDetune) {
    normalizedFrequency *= fdlibm_exp2(aDetune / 1200);
  }

  switch (aType) {
    case BiquadFilterType::Lowpass:
      aBiquad.setLowpassParams(normalizedFrequency, aQ);
      break;
    case BiquadFilterType::Highpass:
      aBiquad.setHighpassParams(normalizedFrequency, aQ);
      break;
    case BiquadFilterType::Bandpass:
      aBiquad.setBandpassParams(normalizedFrequency, aQ);
      break;
    case BiquadFilterType::Lowshelf:
      aBiquad.setLowShelfParams(normalizedFrequency, aGain);
      break;
    case BiquadFilterType::Highshelf:
      aBiquad.setHighShelfParams(normalizedFrequency, aGain);
      break;
    case BiquadFilterType::Peaking:
      aBiquad.setPeakingParams(normalizedFrequency, aQ, aGain);
      break;
    case BiquadFilterType::Notch:
      aBiquad.setNotchParams(normalizedFrequency, aQ);
      break;
    case BiquadFilterType::Allpass:
      aBiquad.setAllpassParams(normalizedFrequency, aQ);
      break;
    default:
      MOZ_ASSERT_UNREACHABLE("We should never see the alternate names here");
      break;
  }
}

class BiquadFilterNodeEngine final : public AudioNodeEngine {
 public:
  BiquadFilterNodeEngine(AudioNode* aNode, AudioDestinationNode* aDestination,
                         uint64_t aWindowID)
      : AudioNodeEngine(aNode),
        mDestination(aDestination->Track())
        // Keep the default values in sync with the default values in
        // BiquadFilterNode::BiquadFilterNode
        ,
        mType(BiquadFilterType::Lowpass),
        mFrequency(350.f),
        mDetune(0.f),
        mQ(1.f),
        mGain(0.f),
        mWindowID(aWindowID) {}

  enum Parameters { TYPE, FREQUENCY, DETUNE, Q, GAIN };
  void SetInt32Parameter(uint32_t aIndex, int32_t aValue) override {
    switch (aIndex) {
      case TYPE:
        mType = static_cast<BiquadFilterType>(aValue);
        break;
      default:
        NS_ERROR("Bad BiquadFilterNode Int32Parameter");
    }
  }
  void RecvTimelineEvent(uint32_t aIndex, AudioParamEvent& aEvent) override {
    MOZ_ASSERT(mDestination);

    aEvent.ConvertToTicks(mDestination);

    switch (aIndex) {
      case FREQUENCY:
        mFrequency.InsertEvent<int64_t>(aEvent);
        break;
      case DETUNE:
        mDetune.InsertEvent<int64_t>(aEvent);
        break;
      case Q:
        mQ.InsertEvent<int64_t>(aEvent);
        break;
      case GAIN:
        mGain.InsertEvent<int64_t>(aEvent);
        break;
      default:
        NS_ERROR("Bad BiquadFilterNodeEngine TimelineParameter");
    }
  }

  void ProcessBlock(AudioNodeTrack* aTrack, GraphTime aFrom,
                    const AudioBlock& aInput, AudioBlock* aOutput,
                    bool* aFinished) override {
    TRACE("BiquadFilterNode::ProcessBlock");
    auto alignedInputBuffer = aTrack->GetScratch<float>(aTrack->BlockSize());
    ASSERT_ALIGNED16(alignedInputBuffer.data());

    if (aInput.IsNull()) {
      bool hasTail = false;
      for (uint32_t i = 0; i < mBiquads.Length(); ++i) {
        if (mBiquads[i].hasTail()) {
          hasTail = true;
          break;
        }
      }
      if (!hasTail) {
        if (!mBiquads.IsEmpty()) {
          mBiquads.Clear();
          aTrack->ScheduleCheckForInactive();

          RefPtr<PlayingRefChangeHandler> refchanged =
              new PlayingRefChangeHandler(aTrack,
                                          PlayingRefChangeHandler::RELEASE);
          aTrack->Graph()->DispatchToMainThreadStableState(refchanged.forget());
        }

        aOutput->SetNull(aTrack->BlockSize());
        return;
      }

      PodZero(alignedInputBuffer.data(), aTrack->BlockSize());

    } else if (mBiquads.Length() != aInput.ChannelCount()) {
      if (mBiquads.IsEmpty()) {
        RefPtr<PlayingRefChangeHandler> refchanged =
            new PlayingRefChangeHandler(aTrack,
                                        PlayingRefChangeHandler::ADDREF);
        aTrack->Graph()->DispatchToMainThreadStableState(refchanged.forget());
      } else {  // Help people diagnose bug 924718
        WebAudioUtils::LogToDeveloperConsole(
            mWindowID, "BiquadFilterChannelCountChangeWarning");
      }

      // Adjust the number of biquads based on the number of channels
      mBiquads.SetLength(aInput.ChannelCount());
    }

    uint32_t numberOfChannels = mBiquads.Length();
    aOutput->AllocateChannels(numberOfChannels);

    TrackTime pos = mDestination->GraphTimeToTrackTime(aFrom);

    uint32_t blockSize = aTrack->BlockSize();
    double freq = mFrequency.GetValueAtTime(pos, blockSize);
    double q = mQ.GetValueAtTime(pos, blockSize);
    double gain = mGain.GetValueAtTime(pos, blockSize);
    double detune = mDetune.GetValueAtTime(pos, blockSize);

    for (uint32_t i = 0; i < numberOfChannels; ++i) {
      const float* input;
      if (aInput.IsNull()) {
        input = alignedInputBuffer.data();
      } else {
        input = static_cast<const float*>(aInput.mChannelData[i]);