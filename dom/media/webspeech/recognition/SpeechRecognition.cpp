/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognition.h"
#include "SpeechRecognitionBackend.h"

#include <algorithm>

#include "AudioSegment.h"
#include "CubebUtils.h"
#include "MediaEnginePrefs.h"
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
#include "SpeechRecognitionResultList.h"
#include "SpeechRecognitionResult.h"
#include "SpeechRecognitionAlternative.h"
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
      mAborted(false),
      mSpeechGrammarList(new SpeechGrammarList(GetOwnerGlobal())),
      mContinuous(false),
      mCurrentState(FSMState::STATE_IDLE),
      mInterimResults(false),
      mState(FSMState::STATE_IDLE),
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

bool SpeechRecognition::StateBetween(FSMState begin, FSMState end) {
  return mCurrentState >= begin && mCurrentState <= end;
}

void SpeechRecognition::SetState(FSMState state) {
  mCurrentState = state;
  LOG("Transitioned to state %s", GetName(mCurrentState));
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

void SpeechRecognition::ProcessEvent(SpeechEvent* aEvent) {
  MOZ_ASSERT(NS_IsMainThread(), "ProcessEvent must be on main thread");
  LOG("Processing %s, current state is %s", GetName(aEvent),
      GetName(mCurrentState));

  if (mAborted && aEvent->mType != EVENT_ABORT) {
    // ignore all events while aborting
    return;
  }

  Transition(aEvent);
}

void SpeechRecognition::Transition(SpeechEvent* aEvent) {
  switch (mCurrentState) {
    case STATE_IDLE:
      switch (aEvent->mType) {
        case EVENT_START:
          // TODO: may want to time out if we wait too long
          // for user to approve
          WaitForAudioData(aEvent);
          break;
        case EVENT_STOP:
        case EVENT_ABORT:
        case EVENT_AUDIO_DATA:
        case EVENT_RECOGNITIONSERVICE_INTERMEDIATE_RESULT:
        case EVENT_RECOGNITIONSERVICE_FINAL_RESULT:
          DoNothing(aEvent);
          break;
        case EVENT_AUDIO_ERROR:
        case EVENT_RECOGNITIONSERVICE_ERROR:
          AbortError(aEvent);
          break;
        default:
          MOZ_CRASH("Invalid event");
      }
      break;
    case STATE_STARTING:
      switch (aEvent->mType) {
        case EVENT_AUDIO_DATA:
          StartedAudioCapture(aEvent);
          break;
        case EVENT_AUDIO_ERROR:
        case EVENT_RECOGNITIONSERVICE_ERROR:
          AbortError(aEvent);
          break;
        case EVENT_ABORT:
          break;
        case EVENT_STOP:
          ResetAndEnd();
          break;
        case EVENT_RECOGNITIONSERVICE_INTERMEDIATE_RESULT:
        case EVENT_RECOGNITIONSERVICE_FINAL_RESULT:
          DoNothing(aEvent);
          break;
        case EVENT_START:
          LOG("STATE_STARTING: Unhandled event %s", GetName(aEvent));
          break;
        default:
          MOZ_CRASH("Invalid event");
      }
      break;
    case STATE_ESTIMATING:
      switch (aEvent->mType) {
        case EVENT_AUDIO_DATA:
          WaitForEstimation(aEvent);
          break;
        case EVENT_STOP:
          StopRecordingAndRecognize(aEvent);
          break;
        case EVENT_ABORT:
          break;
        case EVENT_RECOGNITIONSERVICE_INTERMEDIATE_RESULT:
        case EVENT_RECOGNITIONSERVICE_FINAL_RESULT:
        case EVENT_RECOGNITIONSERVICE_ERROR:
          DoNothing(aEvent);
          break;
        case EVENT_AUDIO_ERROR:
          AbortError(aEvent);
          break;
        case EVENT_START:
          LOG("STATE_ESTIMATING: Unhandled event {}",
              static_cast<int>(aEvent->mType));
          break;
        default:
          MOZ_CRASH("Invalid event");
      }
      break;
    case STATE_WAITING_FOR_SPEECH:
      switch (aEvent->mType) {
        case EVENT_AUDIO_DATA:
          DetectSpeech(aEvent);
          break;
        case EVENT_STOP:
          StopRecordingAndRecognize(aEvent);
          break;
        case EVENT_ABORT:
          break;
        case EVENT_AUDIO_ERROR:
          AbortError(aEvent);
          break;
        case EVENT_RECOGNITIONSERVICE_INTERMEDIATE_RESULT:
        case EVENT_RECOGNITIONSERVICE_FINAL_RESULT:
        case EVENT_RECOGNITIONSERVICE_ERROR:
          DoNothing(aEvent);
          break;
        case EVENT_START:
          LOG("STATE_STARTING: Unhandled event %s", GetName(aEvent));
          break;
        default:
          MOZ_CRASH("Invalid event");
      }
      break;
    case STATE_RECOGNIZING:
      switch (aEvent->mType) {
        case EVENT_AUDIO_DATA:
          WaitForSpeechEnd(aEvent);
          break;
        case EVENT_STOP:
          StopRecordingAndRecognize(aEvent);
          break;
        case EVENT_AUDIO_ERROR:
        case EVENT_RECOGNITIONSERVICE_ERROR:
          AbortError(aEvent);
          break;
        case EVENT_ABORT:
          break;
        case EVENT_RECOGNITIONSERVICE_FINAL_RESULT:
        case EVENT_RECOGNITIONSERVICE_INTERMEDIATE_RESULT:
          DoNothing(aEvent);
          break;
        case EVENT_START:
          LOG("STATE_RECOGNIZING: Unhandled aEvent %s", GetName(aEvent));
          break;
        default:
          LOG("Not handled ?");
          break;
      }
      break;
    case STATE_WAITING_FOR_RESULT:
      switch (aEvent->mType) {
        case EVENT_STOP:
          DoNothing(aEvent);
          break;
        case EVENT_AUDIO_ERROR:
        case EVENT_RECOGNITIONSERVICE_ERROR:
          AbortError(aEvent);
          break;
        case EVENT_RECOGNITIONSERVICE_FINAL_RESULT:
          NotifyFinalResult(aEvent);
          break;
        case EVENT_AUDIO_DATA:
          DoNothing(aEvent);
          break;
        case EVENT_ABORT:
          break;
        case EVENT_START:
        case EVENT_RECOGNITIONSERVICE_INTERMEDIATE_RESULT:
          LOG("STATE_WAITING_FOR_RESULT: Unhandled aEvent %s", GetName(aEvent));
          break;
        default:
          LOG("Not handled");
          break;
      }
      break;
    case STATE_ABORTING:
      switch (aEvent->mType) {
        case EVENT_STOP:
        case EVENT_ABORT:
        case EVENT_AUDIO_DATA:
        case EVENT_AUDIO_ERROR:
        case EVENT_RECOGNITIONSERVICE_INTERMEDIATE_RESULT:
        case EVENT_RECOGNITIONSERVICE_FINAL_RESULT:
        case EVENT_RECOGNITIONSERVICE_ERROR:
          DoNothing(aEvent);
          break;
        case EVENT_START:
          LOG("STATE_ABORTING: Unhandled aEvent %s", GetName(aEvent));
          break;
        default:
          LOG("Invalid event");
      }
      break;
    default:
      LOG("Invalid state");
  }
}

/****************************************************************************
 * FSM Transition functions
 *
 * If a transition function may cause a DOM event to be fired,
 * it may also be re-entered, since the event handler may cause the
 * event loop to spin and new SpeechEvents to be processed.
 *
 * Rules:
 * 1) These methods should call SetState as soon as possible.
 * 2) If these methods dispatch DOM events, or call methods that dispatch
 * DOM events, that should be done as late as possible.
 * 3) If anything must happen after dispatching a DOM event, make sure
 * the state is still what the method expected it to be.
 ****************************************************************************/

void SpeechRecognition::Reset() {
  MOZ_ASSERT(NS_IsMainThread(), "Reset must be on main thread");
  SetState(STATE_IDLE);
  mTrack = nullptr;
  mStopRecordingPromise = nullptr;
  mSpeechDetectionTimer->Cancel();
  mAborted = false;
}

void SpeechRecognition::ResetAndEnd() {
  Reset();
  DispatchTrustedEvent(u"end"_ns);
}

void SpeechRecognition::WaitForAudioData(SpeechEvent* aEvent) {
  SetState(STATE_STARTING);
}

void SpeechRecognition::StartedAudioCapture(SpeechEvent* aEvent) {}

void SpeechRecognition::StopRecordingAndRecognize(SpeechEvent* aEvent) {}

void SpeechRecognition::WaitForEstimation(SpeechEvent* aEvent) {}

void SpeechRecognition::DetectSpeech(SpeechEvent* aEvent) {}

void SpeechRecognition::WaitForSpeechEnd(SpeechEvent* aEvent) {}

void SpeechRecognition::NotifyFinalResult(SpeechEvent* aEvent) {
  ResetAndEnd();

  RootedDictionary<SpeechRecognitionEventInit> init(RootingCx());
  init.mBubbles = true;
  init.mCancelable = false;
  // init.mResultIndex = 0;
  init.mResults = aEvent->mRecognitionResultList;
  init.mInterpretation = JS::NullValue();
  // init.mEmma = nullptr;

  RefPtr<SpeechRecognitionEvent> event =
      SpeechRecognitionEvent::Constructor(this, u"result"_ns, init);
  event->SetTrusted(true);

  DispatchEvent(*event);
}

void SpeechRecognition::DoNothing(SpeechEvent* aEvent) {}

void SpeechRecognition::AbortError(SpeechEvent* aEvent) { NotifyError(aEvent); }

void SpeechRecognition::NotifyError(SpeechEvent* aEvent) {
  aEvent->mError->SetTrusted(true);

  DispatchEvent(*aEvent->mError);
}

/**************************************
 * Event triggers and other functions *
 **************************************/
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

void SpeechRecognition::GetServiceURI(nsString& aRetVal,
                                      ErrorResult& aRv) const {
  aRv.Throw(NS_ERROR_NOT_IMPLEMENTED);
}

void SpeechRecognition::SetServiceURI(const nsAString& aArg, ErrorResult& aRv) {
  aRv.Throw(NS_ERROR_NOT_IMPLEMENTED);
}

// New attributes from current spec

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

bool SpeechRecognition::ProcessLocally() const { return true; }

void SpeechRecognition::SetProcessLocally(bool aProcessLocally) {
  // This always processes locally
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
    const GlobalObject& aGlobal, const SpeechRecognitionOptions& aOptions, ErrorResult& aRv
) {

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
    aRv.ThrowInvalidStateError("Document not active for SpeechRecognition::Install");
    return nullptr;
  }

  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());
  if (!global) {
    aRv.Throw(NS_ERROR_FAILURE);
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
  LOG("SpeechRecognition::Start called, current state: %s",
      GetName(mCurrentState));
  if (mCurrentState != STATE_IDLE) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }

  if (mBackend) {
    aRv.ThrowInvalidStateError("Only one recognition session at the same time for now");
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
    graphRate = CubebUtils::PreferredSampleRate(false);
  }

  // init and start the backend
  // Extract phrase strings from our local copy of SpeechRecognitionPhrase objects
  // The backend gets these at Start() time; the spec is unclear on dynamic updates
  nsTArray<nsString> phrasesForBackend;
  for (const auto& phrase : mPhrases) {
    if (phrase) {
      nsString phraseStr;
      phrase->GetPhrase(phraseStr);
      phrasesForBackend.AppendElement(phraseStr);
    }
  }
  mBackend = MakeRefPtr<SpeechRecognitionBackend>(this, graphRate, mLang, phrasesForBackend);
  nsresult rv = mBackend->Start();
  if (NS_FAILED(rv)) {
    LOGE("Failed to start backend: {:x}", static_cast<uint32_t>(rv));
    aRv.Throw(rv);
    return;
  }

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
              if (mAborted || mCurrentState != STATE_STARTING ||
                  mStreamGeneration != generation) {
                // We were probably aborted. Exit early.
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
              if (mAborted || mCurrentState != STATE_STARTING ||
                  mStreamGeneration != generation) {
                // We were probably aborted. Exit early.
                return;
              }
              SpeechRecognitionErrorCode errorCode;

              if (error->mName == MediaMgrError::Name::NotAllowedError) {
                errorCode = SpeechRecognitionErrorCode::Not_allowed;
              } else {
                errorCode = SpeechRecognitionErrorCode::Audio_capture;
              }
              DispatchError(SpeechRecognition::EVENT_AUDIO_ERROR, errorCode,
                            error->mMessage);
            });
  }

  RefPtr<SpeechEvent> event = new SpeechEvent(this, EVENT_START);
  NS_DispatchToMainThread(event);
}

void SpeechRecognition::Stop() {
  MOZ_ASSERT(NS_IsMainThread(), "Stop must be on main thread");
  if (mBackend) {
    mBackend->Stop();
    // Don't null out mBackend here - it may still deliver final results
    // It will be cleaned up in destructor or when creating a new one
  }

  RefPtr<SpeechEvent> event = new SpeechEvent(this, EVENT_STOP);
  NS_DispatchToMainThread(event);
}

void SpeechRecognition::Abort() {
  if (mAborted) {
    return;
  }

  mAborted = true;

  if (mBackend) {
    mBackend->Abort();
    // Clear backend after abort since no more results are expected
    mBackend = nullptr;
  }

  RefPtr<SpeechEvent> event = new SpeechEvent(this, EVENT_ABORT);
  NS_DispatchToMainThread(event);
}

void SpeechRecognition::DataCallback(TrackTime aTime, const AudioChunk& aChunk) {
  // Note: This is called from the graph thread, not main thread
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

void SpeechRecognition::DispatchError(EventType aErrorType,
                                      SpeechRecognitionErrorCode aErrorCode,
                                      const nsACString& aMessage) {
  MOZ_ASSERT(NS_IsMainThread(), "DispatchError must be on main thread");
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aErrorType == EVENT_RECOGNITIONSERVICE_ERROR ||
                 aErrorType == EVENT_AUDIO_ERROR,
             "Invalid error type!");

  RefPtr<SpeechRecognitionError> srError =
      new SpeechRecognitionError(nullptr, nullptr, nullptr);

  srError->InitSpeechRecognitionError(u"error"_ns, true, false, aErrorCode,
                                      aMessage);

  RefPtr<SpeechEvent> event = new SpeechEvent(this, aErrorType);
  event->mError = srError;
  NS_DispatchToMainThread(event);
}


const char* SpeechRecognition::GetName(FSMState aId) {
  static const char* names[] = {
      "STATE_IDLE",        "STATE_STARTING",
      "STATE_ESTIMATING",  "STATE_WAITING_FOR_SPEECH",
      "STATE_RECOGNIZING", "STATE_WAITING_FOR_RESULT",
      "STATE_ABORTING",
  };

  MOZ_ASSERT(aId < STATE_COUNT);
  MOZ_ASSERT(std::size(names) == STATE_COUNT);
  return names[aId];
}

const char* SpeechRecognition::GetName(SpeechEvent* aEvent) {
  static const char* names[] = {"EVENT_START",
                                "EVENT_STOP",
                                "EVENT_ABORT",
                                "EVENT_AUDIO_DATA",
                                "EVENT_AUDIO_ERROR",
                                "EVENT_RECOGNITIONSERVICE_INTERMEDIATE_RESULT",
                                "EVENT_RECOGNITIONSERVICE_FINAL_RESULT",
                                "EVENT_RECOGNITIONSERVICE_ERROR"};

  MOZ_ASSERT(aEvent->mType < EVENT_COUNT);
  MOZ_ASSERT(std::size(names) == EVENT_COUNT);
  return names[aEvent->mType];
}

void SpeechRecognition::HandleRecognitionResultFromBackend(
    const nsCString& aTranscript, bool aIsFinal) {
  MOZ_ASSERT(NS_IsMainThread(), "Must be called on main thread");
  LOG("HandleRecognitionResultFromBackend: {} (final={})", aTranscript.get(), aIsFinal);

  // Check if we're still active (not aborted)
  if (mAborted || !mBackend) {
    LOG("Ignoring result - recognition was aborted or backend is gone");
    return;
  }

  // For now, send all recognition results as requested
  // Create a simple SpeechRecognitionResultList with one result and one alternative
  RefPtr<SpeechRecognitionResultList> resultList =
      new SpeechRecognitionResultList(this);

  RefPtr<SpeechRecognitionResult> result =
      new SpeechRecognitionResult(this);

  RefPtr<SpeechRecognitionAlternative> alternative =
      new SpeechRecognitionAlternative(this);

  // Set the transcript and confidence (use 1.0 for now)
  alternative->mTranscript = NS_ConvertUTF8toUTF16(aTranscript);
  alternative->mConfidence = 1.0f;

  result->mItems.AppendElement(alternative);
  // Note: IsFinal() currently always returns true in SpeechRecognitionResult
  // The aIsFinal parameter is logged but not stored for now

  resultList->mItems.AppendElement(result);

  // Bypass FSM - directly create and dispatch DOM result event
  RootedDictionary<SpeechRecognitionEventInit> init(RootingCx());
  init.mBubbles = true;
  init.mCancelable = false;
  init.mResults = resultList;
  init.mInterpretation = JS::NullValue();

  RefPtr<SpeechRecognitionEvent> domEvent =
      SpeechRecognitionEvent::Constructor(this, u"result"_ns, init);
  domEvent->SetTrusted(true);

  LOG("Dispatching result DOM event directly");
  DispatchEvent(*domEvent);
}

void SpeechRecognition::HandleRecognitionErrorFromBackend(
    const nsCString& aError) {
  MOZ_ASSERT(NS_IsMainThread(), "Must be called on main thread");
  LOGE("HandleRecognitionErrorFromBackend: {}", aError.get());

  // Check if we're still active
  if (mAborted || !mBackend) {
    LOG("Ignoring error - recognition was aborted or backend is gone");
    return;
  }

  // Bypass FSM - directly create and dispatch DOM error event
  RefPtr<SpeechRecognitionError> srError =
      new SpeechRecognitionError(nullptr, nullptr, nullptr);

  // Map backend errors to appropriate error codes
  SpeechRecognitionErrorCode errorCode = SpeechRecognitionErrorCode::Network;
  if (aError.EqualsLiteral("concurrent-session")) {
    // Use service-not-allowed for concurrent session rejection
    errorCode = SpeechRecognitionErrorCode::Service_not_allowed;
  }

  srError->InitSpeechRecognitionError(u"error"_ns, true, false,
                                      errorCode,
                                      aError);
  srError->SetTrusted(true);

  LOG("Dispatching error DOM event directly");
  DispatchEvent(*srError);
}

SpeechEvent::SpeechEvent(SpeechRecognition* aRecognition,
                         SpeechRecognition::EventType aType)
    : Runnable("dom::SpeechEvent"),
      mAudioSegment(nullptr),
      mRecognitionResultList(nullptr),
      mError(nullptr),
      mRecognition(new nsMainThreadPtrHolder<SpeechRecognition>(
          "SpeechEvent::SpeechEvent", aRecognition)),
      mType(aType),
      mTrackRate(0) {}

SpeechEvent::SpeechEvent(nsMainThreadPtrHandle<SpeechRecognition>& aRecognition,
                         SpeechRecognition::EventType aType)
    : Runnable("dom::SpeechEvent"),
      mAudioSegment(nullptr),
      mRecognitionResultList(nullptr),
      mError(nullptr),
      mRecognition(aRecognition),
      mType(aType),
      mTrackRate(0) {}

SpeechEvent::~SpeechEvent() { delete mAudioSegment; }

NS_IMETHODIMP
SpeechEvent::Run() {
  mRecognition->ProcessEvent(this);
  return NS_OK;
}

}  // namespace mozilla::dom
