/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AudioDestinationNode.h"

#include "AlignmentUtils.h"
#include "AudibilityMonitor.h"
#include "AudioContext.h"
#include "AudioNodeEngine.h"
#include "AudioNodeTrack.h"
#include "CubebUtils.h"
#include "MediaTrackGraph.h"
#include "Tracing.h"
#include "mozilla/StaticPrefs_dom.h"
#include "mozilla/dom/AudioDestinationNodeBinding.h"
#include "mozilla/dom/BaseAudioContextBinding.h"
#include "mozilla/dom/OfflineAudioCompletionEvent.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/dom/WakeLock.h"
#include "mozilla/dom/power/PowerManagerService.h"
#include "nsContentUtils.h"
#include "nsGlobalWindowInner.h"

extern mozilla::LazyLogModule gAudioChannelLog;

#define AUDIO_CHANNEL_LOG(msg, ...) \
  MOZ_LOG(gAudioChannelLog, LogLevel::Debug, (msg, ##__VA_ARGS__))

namespace mozilla::dom {

namespace {
class OnCompleteTask final : public Runnable {
 public:
  OnCompleteTask(AudioContext* aAudioContext, AudioBuffer* aRenderedBuffer)
      : Runnable("dom::OfflineDestinationNodeEngine::OnCompleteTask"),
        mAudioContext(aAudioContext),
        mRenderedBuffer(aRenderedBuffer) {}

  NS_IMETHOD Run() override {
    OfflineAudioCompletionEventInit param;
    param.mRenderedBuffer = mRenderedBuffer;

    RefPtr<OfflineAudioCompletionEvent> event =
        OfflineAudioCompletionEvent::Constructor(mAudioContext, u"complete"_ns,
                                                 param);
    mAudioContext->DispatchTrustedEvent(event);

    return NS_OK;
  }

 private:
  RefPtr<AudioContext> mAudioContext;
  RefPtr<AudioBuffer> mRenderedBuffer;
};
}  // anonymous namespace

class OfflineDestinationNodeEngine final : public AudioNodeEngine {
 public:
  explicit OfflineDestinationNodeEngine(AudioDestinationNode* aNode)
      : AudioNodeEngine(aNode),
        mWriteIndex(0),
        mNumberOfChannels(aNode->ChannelCount()),
        mLength(aNode->Length()),
        mSampleRate(aNode->Context()->SampleRate()),
        mBufferAllocated(false) {}

  void ProcessBlock(AudioNodeTrack* aTrack, GraphTime aFrom,
                    const AudioBlock& aInput, AudioBlock* aOutput,
                    bool* aFinished) override {
    TRACE("OfflineDestinationNodeEngine::ProcessBlock");
    // Do this just for the sake of political correctness; this output
    // will not go anywhere.
    *aOutput = aInput;

    // The output buffer is allocated lazily, on the rendering thread, when
    // non-null input is received.
    if (!mBufferAllocated && !aInput.IsNull()) {
      // These allocations might fail if content provides a huge number of
      // channels or size, but it's OK since we'll deal with the failure
      // gracefully.
      mBuffer = ThreadSharedFloatArrayBufferList::Create(mNumberOfChannels,
                                                         mLength, fallible);
      if (mBuffer && mWriteIndex) {
        // Zero leading for any null chunks that were skipped.
        for (uint32_t i = 0; i < mNumberOfChannels; ++i) {
          float* channelData = mBuffer->GetDataForWrite(i);
          PodZero(channelData, mWriteIndex);
        }
      }

      mBufferAllocated = true;
    }

    // Skip copying if there is no buffer.
    uint32_t outputChannelCount = mBuffer ? mNumberOfChannels : 0;

    // Record our input buffer
    MOZ_ASSERT(mWriteIndex < mLength, "How did this happen?");
    const uint32_t duration =
        std::min(aTrack->BlockSize(), mLength - mWriteIndex);
    const uint32_t inputChannelCount = aInput.ChannelCount();
    for (uint32_t i = 0; i < outputChannelCount; ++i) {
      float* outputData = mBuffer->GetDataForWrite(i) + mWriteIndex;
      if (aInput.IsNull() || i >= inputChannelCount) {
        PodZero(outputData, duration);
      } else {
        const float* inputBuffer =
            static_cast<const float*>(aInput.mChannelData[i]);
        if (duration == aTrack->BlockSize() && IS_ALIGNED16(inputBuffer)) {
          // Use the optimized version of the copy with scale operation