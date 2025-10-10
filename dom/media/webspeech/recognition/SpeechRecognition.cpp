/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognition.h"

#include <algorithm>

#include "AudioSegment.h"
#include "CubebUtils.h"
#include "MediaEnginePrefs.h"
#include "SpeechRecognitionAlternative.h"
#include "SpeechRecognitionBackend.h"
#include "SpeechRecognitionResult.h"
#include "SpeechRecognitionResultList.h"
#include "SpeechTrackListener.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/MediaManager.h"
#include "mozilla/Preferences.h"
#include "mozilla/ResultVariant.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/dom/AudioStreamTrack.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/MediaStreamError.h"
#include "mozilla/dom/MediaStreamTrackBinding.h"
#include "mozilla/dom/RootedDictionary.h"
#include "mozilla/dom/SpeechGrammar.h"
#include "mozilla/dom/SpeechRecognitionBinding.h"
#include "mozilla/dom/SpeechRecognitionEvent.h"
#include "mozilla/intl/Locale.h"
#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsContentUtils.h"
#include "nsCycleCollectionParticipant.h"
#include "nsGlobalWindowInner.h"
#include "nsIContent.h"
#include "nsIPermissionManager.h"
#include "nsIPrincipal.h"
#include "nsPIDOMWindow.h"
#include "nsQueryObject.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"

// Undo the windows.h damage
#if defined(XP_WIN) && defined(GetMessage)
#  undef GetMessage
#endif

namespace mozilla::dom {
using mozilla::CubebUtils::PreferredSampleRate;

static LazyLogModule gSpeechRecognitionLog("SpeechRecognition");

#define LOG(fmt, ...)                                               \
  MOZ_LOG_FMT(gSpeechRecognitionLog, mozilla::LogLevel::Debug, fmt, \
              ##__VA_ARGS__)
#define LOGV(fmt, ...)                                                \
  MOZ_LOG_FMT(gSpeechRecognitionLog, mozilla::LogLevel::Verbose, fmt, \
              ##__VA_ARGS__)
#define LOGE(fmt, ...)                                              \
  MOZ_LOG_FMT(gSpeechRecognitionLog, mozilla::LogLevel::Error, fmt, \
              ##__VA_ARGS__)

NS_IMPL_CYCLE_COLLECTION_WEAK_PTR_INHERITED(SpeechRecognition,
                                            DOMEventTargetHelper, mTrack,
                                            mSpeechGrammarList, mListener,
                                            mBackend, mPhrases)

NS_IMPL_ADDREF_INHERITED(SpeechRecognition, DOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(SpeechRecognition, DOMEventTargetHelper)

NS_IMPL_CYCLE_COLLECTION_INHERITED(SpeechRecognition::TrackListener,
                                   DOMMediaStream::TrackListener,
                                   mSpeechRecognition)
NS_IMPL_ADDREF_INHERITED(SpeechRecognition::TrackListener,
                         DOMMediaStream::TrackListener)
NS_IMPL_RELEASE_INHERITED(SpeechRecognition::TrackListener,
                          DOMMediaStream::TrackListener)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(SpeechRecognition::TrackListener)
NS_INTERFACE_MAP_END_INHERITING(DOMMediaStream::TrackListener)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(SpeechRecognition)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)

SpeechRecognition::SpeechRecognition(nsPIDOMWindowInner* aOwnerWindow)
    : DOMEventTargetHelper(aOwnerWindow),
      mSpeechDetectionTimer(NS_NewTimer()),
      mStarted(false),
      mSpeechGrammarList(new SpeechGrammarList(GetOwnerGlobal())),
      mContinuous(false),
      mInterimResults(false),
      mMaxAlternatives(1),
      mProcessLocally(false) {
  LOG("SpeechRecognition::SpeechRecognition");

  Reset();
}

SpeechRecognition::~SpeechRecognition() {
  MOZ_ASSERT(NS_IsMainThread(), "Destructor must be on main thread");
  LOG("SpeechRecognition::~SpeechRecognition");

  // Ensure backend is properly cleaned up
  if (mBackend) {
    mBackend->Abort();
    mBackend = nullptr;
  }
}

JSObject* SpeechRecognition::WrapObject(JSContext* aCx,
                                        JS::Handle<JSObject*> aGivenProto) {
  return SpeechRecognition_Binding::Wrap(aCx, this, aGivenProto);
}

already_AddRefed<SpeechRecognition> SpeechRecognition::Constructor(
    const GlobalObject& aGlobal, ErrorResult& aRv) {
  nsCOMPtr<nsPIDOMWindowInner> win = do_QueryInterface(aGlobal.GetAsSupports());
  if (!win) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  RefPtr<SpeechRecognition> object = new SpeechRecognition(win);
  return object.forget();
}

void SpeechRecognition::Reset() {
  MOZ_ASSERT(NS_IsMainThread(), "Reset must be on main thread");
  mStarted = false;
  mTrack = nullptr;
  mStopRecordingPromise = nullptr;
  mSpeechDetectionTimer->Cancel();
}

void SpeechRecognition::ResetAndEnd() {
  Reset();
  DispatchTrustedEvent(u"end"_ns);
}

NS_IMETHODIMP
SpeechRecognition::StartRecording(RefPtr<AudioStreamTrack>& aTrack) {
  // hold a reference so that the underlying track doesn't get collected.
  mTrack = aTrack;
  MOZ_ASSERT(!mTrack->Ended());

  mSpeechListener = SpeechTrackListener::Create(this);
  mTrack->AddListener(mSpeechListener);

  return NS_OK;
}

RefPtr<GenericNonExclusivePromise> SpeechRecognition::StopRecording() {
  if (!mTrack) {
    // Recording wasn't started, or has already been stopped.
    if (mTrack) {
      mListener = nullptr;
    }
    return GenericNonExclusivePromise::CreateAndResolve(true, __func__);
  }

  if (mStopRecordingPromise) {
    return mStopRecordingPromise;
  }

  mTrack->RemoveListener(mSpeechListener);
  if (mTrackIsOwned) {
    mTrack->Stop();
  }

  DispatchTrustedEvent(u"audioend"_ns);

  return nullptr;
}

already_AddRefed<SpeechGrammarList> SpeechRecognition::Grammars() const {
  RefPtr<SpeechGrammarList> speechGrammarList = mSpeechGrammarList;
  return speechGrammarList.forget();
}

void SpeechRecognition::SetGrammars(SpeechGrammarList& aArg) {
  mSpeechGrammarList = &aArg;
}

void SpeechRecognition::GetLang(nsString& aRetVal) const { aRetVal = mLang; }

void SpeechRecognition::SetLang(const nsAString& aArg) { mLang = aArg; }

bool SpeechRecognition::GetContinuous(ErrorResult& aRv) const {
  return mContinuous;
}

void SpeechRecognition::SetContinuous(bool aArg, ErrorResult& aRv) {
  mContinuous = aArg;
}

bool SpeechRecognition::InterimResults() const { return mInterimResults; }

void SpeechRecognition::SetInterimResults(bool aArg) { mInterimResults = aArg; }

uint32_t SpeechRecognition::MaxAlternatives() const { return mMaxAlternatives; }

void SpeechRecognition::SetMaxAlternatives(uint32_t aArg) {
  mMaxAlternatives = aArg;
}

static bool ValidateBCP47Language(const nsAString& aLang, ErrorResult& aRv) {
  NS_ConvertUTF16toUTF8 utf8Lang(aLang);
  mozilla::Span<const char> langSpan(utf8Lang.get(), utf8Lang.Length());

  // Empty strings are not valid BCP47 language tags
  if (langSpan.IsEmpty()) {
    aRv.ThrowSyntaxError("Invalid BCP47 language tag");
    return false;
  }

  mozilla::intl::Locale locale;
  auto result = mozilla::intl::LocaleParser::TryParse(langSpan, locale);

  if (result.isErr()) {
    aRv.ThrowSyntaxError("Invalid BCP47 language tag");
    return false;
  }

  return true;
}

bool SpeechRecognition::ProcessLocally() const {
  // per spec, this should default to false, but Gecko always processes locally.
  // It's likely that we'll amend the spec.
  return true;
}

void SpeechRecognition::SetProcessLocally(bool aProcessLocally) {
  // Gecko always processes locally. This could be made to throw if set to
  // something not supported, but we need to amend the spec.
}

void SpeechRecognition::OnSetPhrases(SpeechRecognitionPhrase& aPhrase,
                                     uint32_t aIndex, ErrorResult& aRv) {
  // Note: The spec is unclear on whether dynamic updates during recognition
  // should affect ongoing recognition. For now, the backend only gets phrases
  // at Start() time.
  mPhrases.InsertElementAt(aIndex, &aPhrase);
}

void SpeechRecognition::OnDeletePhrases(SpeechRecognitionPhrase& aPhrase,
                                        uint32_t aIndex, ErrorResult& aRv) {
  MOZ_ASSERT(mPhrases.ElementAt(aIndex) == &aPhrase);
  // Similar comment as OnSetPhrases here: changes aren't sent to the backend
  // after start().
  mPhrases.RemoveElementAt(aIndex);
}

/* static */
already_AddRefed<Promise> SpeechRecognition::Available(
    const GlobalObject& aGlobal, const SpeechRecognitionOptions& aOptions,
    ErrorResult& aRv) {
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());
  if (!global) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  // Not specced yet:
  // https://github.com/WebAudio/web-speech-api/issues/174
  if (aOptions.mLangs.IsEmpty()) {
    aRv.ThrowRangeError("empty lang");
    return nullptr;
  }

  // Validate all language tags according to spec
  for (const nsString& lang : aOptions.mLangs) {
    if (!ValidateBCP47Language(lang, aRv)) {
      return nullptr;
    }
    // Check if error was thrown and return early
    if (aRv.Failed()) {
      return nullptr;
    }
  }

  // Convert options to language array and delegate to backend
  nsTArray<nsString> languages;
  for (const nsString& lang : aOptions.mLangs) {
    languages.AppendElement(lang);
  }

  return SpeechRecognitionBackend::Available(global, languages);
}

/* static */
already_AddRefed<Promise> SpeechRecognition::Install(
    const GlobalObject& aGlobal, const SpeechRecognitionOptions& aOptions,
    ErrorResult& aRv) {
  nsCOMPtr<nsPIDOMWindowInner> window =
      do_QueryInterface(aGlobal.GetAsSupports());
  if (!window) {
    aRv.ThrowAbortError("No global object for SpeechRecognition::Install");
    return nullptr;
  }

  nsCOMPtr<Document> doc = window->GetExtantDoc();
  if (!doc) {
    aRv.ThrowAbortError("No document for SpeechRecognition::Install");
    return nullptr;
  }

  if (!doc->IsCurrentActiveDocument()) {
    aRv.ThrowInvalidStateError(
        "Document not active for SpeechRecognition::Install");
    return nullptr;
  }

  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());
  if (!global) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  // Not specced yet:
  // https://github.com/WebAudio/web-speech-api/issues/174
  if (aOptions.mLangs.IsEmpty()) {
    aRv.ThrowRangeError("empty lang");
    return nullptr;
  }

  // Validate all language tags according to spec
  for (const nsString& lang : aOptions.mLangs) {
    if (!ValidateBCP47Language(lang, aRv)) {
      return nullptr;
    }
    // Check if error was thrown and return early
    if (aRv.Failed()) {
      return nullptr;
    }
  }

  // Convert options to language array and delegate to backend
  nsTArray<nsString> languages;
  for (const nsString& lang : aOptions.mLangs) {
    languages.AppendElement(lang);
  }

  return SpeechRecognitionBackend::Install(global, languages);
}

void SpeechRecognition::Start(
    const Optional<NonNull<MediaStreamTrack>>& aAudioTrack,
    CallerType aCallerType, ErrorResult& aRv) {
  LOG("SpeechRecognition::Start called");

  // Check if already started (spec's [[started]] internal slot)
  if (mStarted) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }

  if (mBackend) {
    aRv.ThrowInvalidStateError(
        "Only one recognition session at the same time for now");
    return;
  }

  MOZ_ASSERT(!mListener);

  // Clean up any existing backend before creating a new one
  if (mBackend) {
    mBackend->Abort();
    mBackend = nullptr;
  }

  uint32_t graphRate = 0;
  if (aAudioTrack.WasPassed()) {
    graphRate = aAudioTrack.Value().Graph()->GraphRate();
  } else {
    // If using the microphone, it is always at the preferred rate
    graphRate = CubebUtils::PreferredSampleRate(false);
  }

  // init and start the backend
  // Extract phrase strings from our local copy of SpeechRecognitionPhrase
  // objects. The backend gets these at Start() time; the spec is unclear on
  // dynamic updates
  nsTArray<nsString> phrasesForBackend;
  for (const auto& phrase : mPhrases) {
    if (phrase) {
      nsString phraseStr;
      phrase->GetPhrase(phraseStr);
      phrasesForBackend.AppendElement(phraseStr);
    }
  }
  mBackend = MakeRefPtr<SpeechRecognitionBackend>(this, graphRate, mLang,
                                                  phrasesForBackend);
  nsresult rv = mBackend->Start();
  if (NS_FAILED(rv)) {
    LOGE("Failed to start backend: {:x}", static_cast<uint32_t>(rv));
    aRv.Throw(rv);
    return;
  }

  // Set started flag per spec
  mStarted = true;

  // Fire start event
  DispatchTrustedEvent(u"start"_ns);

  // MediaStreamTrack (argument passed) vs. Microphone (no argument passed)
  if (aAudioTrack.WasPassed()) {
    RefPtr<MediaStreamTrack> track = &aAudioTrack.Value();
    RefPtr<AudioStreamTrack> audioTrack = track->AsAudioStreamTrack();

    if (!audioTrack) {
      aRv.ThrowTypeError("MediaStreamTrack must be an audio track");
      return;
    }

    if (audioTrack->Ended()) {
      aRv.ThrowInvalidStateError("MediaStreamTrack is ended");
      return;
    }

    NotifyTrackAdded(audioTrack);
  } else {
    MediaStreamConstraints constraints;
    constraints.mAudio.SetAsBoolean() = true;

    nsPIDOMWindowInner* win = GetOwnerWindow();
    if (!win || !win->IsFullyActive()) {
      aRv.ThrowInvalidStateError("The document is not fully active.");
      return;
    }
    AutoNoJSAPI nojsapi;
    RefPtr<SpeechRecognition> self(this);
    MediaManager::Get()
        ->GetUserMedia(win, constraints, aCallerType)
        ->Then(
            GetCurrentSerialEventTarget(), __func__,
            [this, self,
             generation = mStreamGeneration](RefPtr<DOMMediaStream>&& aStream) {
              nsTArray<RefPtr<AudioStreamTrack>> tracks;
              aStream->GetAudioTracks(tracks);
              if (!mStarted || mStreamGeneration != generation) {
                // Recognition was stopped. Exit early.
                for (const RefPtr<AudioStreamTrack>& track : tracks) {
                  track->Stop();
                }
                return;
              }
              mStream = std::move(aStream);
              mStream->RegisterTrackListener(mListener);
              for (const RefPtr<AudioStreamTrack>& track : tracks) {
                if (!track->Ended()) {
                  NotifyTrackAdded(track);
                }
              }
            },
            [this, self,
             generation = mStreamGeneration](RefPtr<MediaMgrError>&& error) {
              if (!mStarted || mStreamGeneration != generation) {
                // Recognition was stopped. Exit early.
                return;
              }
              SpeechRecognitionErrorCode errorCode;

              if (error->mName == MediaMgrError::Name::NotAllowedError) {
                errorCode = SpeechRecognitionErrorCode::Not_allowed;
              } else {
                errorCode = SpeechRecognitionErrorCode::Audio_capture;
              }
              DispatchError(errorCode, error->mMessage);
            });
  }
}

void SpeechRecognition::Stop() {
  // If not started, ignore, per spec
  if (!mStarted) {
    return;
  }

  if (mBackend) {
    // Stop the backend/session. This will dispatch soundend (if needed) and
    // audioend via main thread runnables.
    mBackend->Stop();
    // We are conforming to spec semantics for stop(): finalize recognition and
    // fire 'end'. Clear the backend to avoid late results after end.
    mBackend = nullptr;

    // Ensure 'end' fires after the already-posted 'audioend' runnable from the
    // backend. Post a task to call ResetAndEnd() on the next turn.
    RefPtr<SpeechRecognition> self = this;
    NS_DispatchToMainThread(NS_NewRunnableFunction(
        "SpeechRecognition::FinalizeStop",
        [self = std::move(self)]() { self->ResetAndEnd(); }));
  }
}

void SpeechRecognition::Abort() {
  // If not started, ignore per spec
  if (!mStarted) {
    return;
  }

  if (mBackend) {
    mBackend->Abort();
    // Clear backend after abort since no more results are expected
    mBackend = nullptr;
  }

  // Fire end event and reset
  ResetAndEnd();
}

void SpeechRecognition::DataCallback(TrackTime aTime,
                                     const AudioChunk& aChunk) {
  // Note: This is called from the graph thread, not main thread.
  MOZ_ASSERT(!NS_IsMainThread(), "DataCallback must NOT be on main thread");
  // Delegate to backend
  if (mBackend) {
    mBackend->DataCallback(aTime, aChunk);
  }
}

void SpeechRecognition::NotifyTrackAdded(
    const RefPtr<MediaStreamTrack>& aTrack) {
  if (mTrack) {
    return;
  }

  RefPtr<AudioStreamTrack> audioTrack = aTrack->AsAudioStreamTrack();
  if (!audioTrack) {
    return;
  }

  if (audioTrack->Ended()) {
    return;
  }

  StartRecording(audioTrack);
}

void SpeechRecognition::DispatchError(SpeechRecognitionErrorCode aErrorCode,
                                      const nsACString& aMessage) {
  MOZ_ASSERT(NS_IsMainThread(), "DispatchError must be on main thread");

  RefPtr<SpeechRecognitionError> srError =
      new SpeechRecognitionError(nullptr, nullptr, nullptr);

  srError->InitSpeechRecognitionError(u"error"_ns, true, false, aErrorCode,
                                      aMessage);
  srError->SetTrusted(true);

  DispatchEvent(*srError);
}

void SpeechRecognition::HandleRecognitionResultFromBackend(
    const nsCString& aTranscript, bool aIsFinal) {
  MOZ_ASSERT(NS_IsMainThread(), "Must be called on main thread");
  LOG("HandleRecognitionResultFromBackend: {} (final={})",
      aTranscript.get(), aIsFinal);

  // Check if still active
  if (!mBackend) {
    LOG("Ignoring result - backend is gone");
    return;
  }

  // Create a simple SpeechRecognitionResultList with one result and one
  // alternative. Our backend doesn't support multiple alternatives, but could
  // to support them. It's however already more precise that when the spec was
  // authored so it might be useless to change this.
  RefPtr<SpeechRecognitionResultList> resultList =
      new SpeechRecognitionResultList(this);

  RefPtr<SpeechRecognitionResult> result = new SpeechRecognitionResult(this);

  RefPtr<SpeechRecognitionAlternative> alternative =
      new SpeechRecognitionAlternative(this);

  alternative->mTranscript = NS_ConvertUTF8toUTF16(aTranscript);
  // The confidence is for now always 1.0. We have per token confidence score,
  // and we need the spec to define how to compute this number in an
  // engine-independant way, and for text segment and not per token (e.g.
  // average, median, take lowest for a conservative estimate, etc.).
  alternative->mConfidence = 1.0f;

  result->mItems.AppendElement(alternative);

  result->SetFinal(aIsFinal);

  resultList->mItems.AppendElement(result);

  RootedDictionary<SpeechRecognitionEventInit> init(RootingCx());
  init.mBubbles = true;
  init.mCancelable = false;
  init.mResults = resultList;
  init.mInterpretation = JS::NullValue();

  RefPtr<SpeechRecognitionEvent> domEvent =
      SpeechRecognitionEvent::Constructor(this, u"result"_ns, init);
  domEvent->SetTrusted(true);
  DispatchEvent(*domEvent);
}

void SpeechRecognition::HandleRecognitionErrorFromBackend(
    const nsCString& aError) {
  MOZ_ASSERT(NS_IsMainThread(), "Must be called on main thread");
  LOGE("HandleRecognitionErrorFromBackend: {}", aError.get());

  // Check if we're still active
  if (!mBackend) {
    LOG("Ignoring error - backend is gone");
    return;
  }

  RefPtr<SpeechRecognitionError> srError =
      new SpeechRecognitionError(nullptr, nullptr, nullptr);

  // Map backend errors to appropriate error codes
  SpeechRecognitionErrorCode errorCode = SpeechRecognitionErrorCode::Network;
  if (aError.EqualsLiteral("concurrent-session")) {
    // Use service-not-allowed for concurrent session rejection
    errorCode = SpeechRecognitionErrorCode::Service_not_allowed;
  }

  srError->InitSpeechRecognitionError(u"error"_ns, true, false, errorCode,
                                      aError);
  srError->SetTrusted(true);

  LOG("Dispatching error DOM event directly");
  DispatchEvent(*srError);
}

}  // namespace mozilla::dom
