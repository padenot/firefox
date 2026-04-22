/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IIRFilterNode.h"

#include "AlignmentUtils.h"
#include "AudioDestinationNode.h"
#include "AudioNodeEngine.h"
#include "AudioNodeTrack.h"
#include "PlayingRefChangeHandler.h"
#include "Tracing.h"
#include "blink/IIRFilter.h"
#include "nsGlobalWindowInner.h"
#include "nsPrintfCString.h"

namespace mozilla::dom {

class IIRFilterNodeEngine final : public AudioNodeEngine {
 public:
  IIRFilterNodeEngine(AudioNode* aNode, AudioDestinationNode* aDestination,
                      const AudioDoubleArray& aFeedforward,
                      const AudioDoubleArray& aFeedback, uint64_t aWindowID)
      : AudioNodeEngine(aNode),
        mDestination(aDestination->Track()),
        mFeedforward(aFeedforward.Clone()),
        mFeedback(aFeedback.Clone()),
        mWindowID(aWindowID) {}

  void ProcessBlock(AudioNodeTrack* aTrack, GraphTime aFrom,
                    const AudioBlock& aInput, AudioBlock* aOutput,
                    bool* aFinished) override {
    TRACE("IIRFilterNodeEngine::ProcessBlock");
    auto alignedInputBuffer = aTrack->GetScratch<float>(aTrack->BlockSize());
    ASSERT_ALIGNED16(alignedInputBuffer.data());

    if (aInput.IsNull()) {
      if (!mIIRFilters.IsEmpty()) {
        bool allZero = true;
        for (uint32_t i = 0; i < mIIRFilters.Length(); ++i) {
          allZero &= mIIRFilters[i]->buffersAreZero();
        }

        // all filter buffer values are zero, so the output will be zero
        // as well.
        if (allZero) {
          mIIRFilters.Clear();
          aTrack->ScheduleCheckForInactive();

          RefPtr<PlayingRefChangeHandler> refchanged =
              new PlayingRefChangeHandler(aTrack,
                                          PlayingRefChangeHandler::RELEASE);
          aTrack->Graph()->DispatchToMainThreadStableState(refchanged.forget());

          aOutput->SetNull(aTrack->BlockSize());
          return;
        }

        PodZero(alignedInputBuffer.data(), aTrack->BlockSize());
      }
    } else if (mIIRFilters.Length() != aInput.ChannelCount()) {
      if (mIIRFilters.IsEmpty()) {
        RefPtr<PlayingRefChangeHandler> refchanged =
            new PlayingRefChangeHandler(aTrack,
                                        PlayingRefChangeHandler::ADDREF);
        aTrack->Graph()->DispatchToMainThreadStableState(refchanged.forget());
      } else {
        WebAudioUtils::LogToDeveloperConsole(
            mWindowID, "IIRFilterChannelCountChangeWarning");
      }

      // Adjust the number of filters based on the number of channels
      mIIRFilters.SetLength(aInput.ChannelCount());
      for (size_t i = 0; i < aInput.ChannelCount(); ++i) {
        mIIRFilters[i] =
            MakeUnique<blink::IIRFilter>(&mFeedforward, &mFeedback);
      }
    }

    uint32_t numberOfChannels = mIIRFilters.Length();
    aOutput->AllocateChannels(numberOfChannels);

    for (uint32_t i = 0; i < numberOfChannels; ++i) {
      const float* input;
      if (aInput.IsNull()) {
        input = alignedInputBuffer.data();
      } else {
        input = static_cast<const float*>(aInput.mChannelData[i]);