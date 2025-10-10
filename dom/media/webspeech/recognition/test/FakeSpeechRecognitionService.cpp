/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FakeSpeechRecognitionService.h"

#include "SpeechRecognition.h"
#include "SpeechRecognitionAlternative.h"
#include "SpeechRecognitionResult.h"
#include "SpeechRecognitionResultList.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/dom/SpeechRecognitionEvent.h"
#include "nsIObserverService.h"
#include "nsThreadUtils.h"

namespace mozilla {

using namespace dom;

NS_IMPL_ISUPPORTS(FakeSpeechRecognitionService, nsISpeechRecognitionService,
                  nsIObserver)

FakeSpeechRecognitionService::FakeSpeechRecognitionService() = default;

FakeSpeechRecognitionService::~FakeSpeechRecognitionService() = default;

NS_IMETHODIMP
FakeSpeechRecognitionService::Initialize(
    WeakPtr<SpeechRecognition> aSpeechRecognition) {
  MOZ_ASSERT(NS_IsMainThread());
  mRecognition = aSpeechRecognition;
  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  obs->AddObserver(this, SPEECH_RECOGNITION_TEST_EVENT_REQUEST_TOPIC, false);
  obs->AddObserver(this, SPEECH_RECOGNITION_TEST_END_TOPIC, false);
  return NS_OK;
}

NS_IMETHODIMP
FakeSpeechRecognitionService::ProcessAudioSegment(AudioSegment* aAudioSegment,
                                                  int32_t aSampleRate) {
  MOZ_ASSERT(!NS_IsMainThread());
  return NS_OK;
}

NS_IMETHODIMP
FakeSpeechRecognitionService::SoundEnd() {
  MOZ_ASSERT(NS_IsMainThread());
  return NS_OK;
}

NS_IMETHODIMP
FakeSpeechRecognitionService::ValidateAndSetGrammarList(
    mozilla::dom::SpeechGrammar*, nsISpeechGrammarCompilationCallback*) {
  return NS_OK;
}

NS_IMETHODIMP
FakeSpeechRecognitionService::Abort() {
  MOZ_ASSERT(NS_IsMainThread());
  return NS_OK;
}

NS_IMETHODIMP
FakeSpeechRecognitionService::Observe(nsISupports* aSubject, const char* aTopic,
                                      const char16_t* aData) {
  MOZ_ASSERT(StaticPrefs::media_webspeech_test_fake_recognition_service(),
             "Got request to fake recognition service event, but "
             "media.webspeech.test.fake_recognition_service is not set");

  if (!strcmp(aTopic, SPEECH_RECOGNITION_TEST_END_TOPIC)) {
    nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
    obs->RemoveObserver(this, SPEECH_RECOGNITION_TEST_EVENT_REQUEST_TOPIC);
    obs->RemoveObserver(this, SPEECH_RECOGNITION_TEST_END_TOPIC);

    return NS_OK;
  }

  const nsDependentString eventName = nsDependentString(aData);

  if (eventName.EqualsLiteral("EVENT_RECOGNITIONSERVICE_ERROR")) {
    mRecognition->DispatchError(
        SpeechRecognitionErrorCode::Network,  // TODO different codes?
        "RECOGNITIONSERVICE_ERROR test event");

  } else if (eventName.EqualsLiteral("EVENT_RECOGNITIONSERVICE_FINAL_RESULT")) {
    // Create and dispatch a result event directly
    RefPtr<SpeechRecognitionResultList> resultList = BuildMockResultList();

    RootedDictionary<SpeechRecognitionEventInit> init(RootingCx());
    init.mBubbles = true;
    init.mCancelable = false;
    init.mResults = resultList;
    init.mInterpretation = JS::NullValue();

    RefPtr<SpeechRecognitionEvent> event =
        SpeechRecognitionEvent::Constructor(mRecognition, u"result"_ns, init);
    event->SetTrusted(true);

    mRecognition->DispatchEvent(*event);
  }
  return NS_OK;
}

SpeechRecognitionResultList*
FakeSpeechRecognitionService::BuildMockResultList() {
  SpeechRecognitionResultList* resultList =
      new SpeechRecognitionResultList(mRecognition);
  SpeechRecognitionResult* result = new SpeechRecognitionResult(mRecognition);
  if (0 < mRecognition->MaxAlternatives()) {
    SpeechRecognitionAlternative* alternative =
        new SpeechRecognitionAlternative(mRecognition);

    alternative->mTranscript = u"Mock final result"_ns;
    alternative->mConfidence = 0.0f;

    result->mItems.AppendElement(alternative);
  }
  resultList->mItems.AppendElement(result);

  return resultList;
}

}  // namespace mozilla
