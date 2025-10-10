/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechTrackListener.h"

#include "SpeechRecognition.h"
#include "nsProxyRelease.h"

namespace mozilla::dom {

SpeechTrackListener::SpeechTrackListener(SpeechRecognition* aRecognition)
    : mRecognition(new nsMainThreadPtrHolder<SpeechRecognition>(
          "SpeechTrackListener::SpeechTrackListener", aRecognition, false)),
      mRemovedPromise(
          mRemovedHolder.Ensure("SpeechTrackListener::mRemovedPromise")) {
  MOZ_ASSERT(NS_IsMainThread());
}

already_AddRefed<SpeechTrackListener> SpeechTrackListener::Create(
    SpeechRecognition* aRecognition) {
  MOZ_ASSERT(NS_IsMainThread());
  RefPtr<SpeechTrackListener> listener = new SpeechTrackListener(aRecognition);

  listener->mRemovedPromise->Then(
      GetCurrentSerialEventTarget(), __func__,
      [listener] { listener->mRecognition = nullptr; });

  return listener.forget();
}

void SpeechTrackListener::NotifyQueuedChanges(
    MediaTrackGraph* aGraph, TrackTime aTrackOffset,
    const MediaSegment& aQueuedMedia) {
  AudioSegment* audio = const_cast<AudioSegment*>(
      static_cast<const AudioSegment*>(&aQueuedMedia));

  TrackTime offsetForChunk = aTrackOffset;
  AudioSegment::ChunkIterator chunk(*audio);
  while (!chunk.IsEnded()) {
    mRecognition->DataCallback(offsetForChunk + chunk->mDuration, *chunk);
    chunk.Next();
  }
}

void SpeechTrackListener::NotifyEnded(MediaTrackGraph* aGraph) {
  // TODO dispatch SpeechEnd event so services can be informed
}

void SpeechTrackListener::NotifyRemoved(MediaTrackGraph* aGraph) {
  mRemovedHolder.ResolveIfExists(true, __func__);
}

}  // namespace mozilla::dom
