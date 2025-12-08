/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITION_H_
#define DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITION_H_

#include "DOMMediaStream.h"
#include "SpeechGrammarList.h"
#include "SpeechRecognitionResultList.h"
#include "js/TypeDecls.h"
#include "mozilla/DOMEventTargetHelper.h"
#include "mozilla/WeakPtr.h"
#include "mozilla/dom/BindingDeclarations.h"
#include "mozilla/dom/SpeechRecognitionBinding.h"
#include "mozilla/dom/SpeechRecognitionErrorBinding.h"
#include "nsCOMPtr.h"
#include "nsProxyRelease.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsTHashSet.h"
#include "nsWrapperCache.h"

namespace mozilla {

namespace dom {

class Promise;
class SpeechRecognitionBackend;
class SpeechRecognitionPhrase;

#define SPEECH_RECOGNITION_TEST_EVENT_REQUEST_TOPIC \
  "SpeechRecognitionTest:RequestEvent"
#define SPEECH_RECOGNITION_TEST_END_TOPIC "SpeechRecognitionTest:End"

class GlobalObject;
class AudioStreamTrack;
class MediaStreamTrack;
class SpeechTrackListener;

class SpeechRecognition final : public DOMEventTargetHelper,
                                public SupportsWeakPtr {
 public:
  MOZ_DECLARE_REFCOUNTED_TYPENAME(SpeechRecognition)

  explicit SpeechRecognition(nsPIDOMWindowInner* aOwnerWindow);

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(SpeechRecognition,
                                           DOMEventTargetHelper)

  JSObject* WrapObject(JSContext* aCx,
                       JS::Handle<JSObject*> aGivenProto) override;

  void DisconnectFromOwner() override;

  static already_AddRefed<SpeechRecognition> Constructor(
      const GlobalObject& aGlobal, ErrorResult& aRv);

  static already_AddRefed<SpeechRecognition> WebkitSpeechRecognition(
      const GlobalObject& aGlobal, ErrorResult& aRv) {
    return Constructor(aGlobal, aRv);
  }

  already_AddRefed<SpeechGrammarList> Grammars() const;

  void SetGrammars(mozilla::dom::SpeechGrammarList& aArg);

  void GetLang(nsString& aRetVal) const;

  void SetLang(const nsAString& aArg);

  bool GetContinuous(ErrorResult& aRv) const;

  void SetContinuous(bool aArg, ErrorResult& aRv);

  bool InterimResults() const;

  void SetInterimResults(bool aArg);

  uint32_t MaxAlternatives() const;

  void SetMaxAlternatives(uint32_t aArg);

  // New attributes from current spec
  bool ProcessLocally() const;
  void SetProcessLocally(bool aProcessLocally);

  // ObservableArray callbacks for phrases
  void OnSetPhrases(SpeechRecognitionPhrase& aPhrase, uint32_t aIndex,
                    ErrorResult& aRv);
  void OnDeletePhrases(SpeechRecognitionPhrase& aPhrase, uint32_t aIndex,
                       ErrorResult& aRv);

  // Static methods from current spec
  static already_AddRefed<Promise> Available(
      const GlobalObject& aGlobal, const SpeechRecognitionOptions& aOptions,
      ErrorResult& aRv);
  static already_AddRefed<Promise> Install(
      const GlobalObject& aGlobal, const SpeechRecognitionOptions& aOptions,
      ErrorResult& aRv);

  static void RemoveDownloadingLanguage(const nsCString& aLanguage);

  void Start(const Optional<NonNull<MediaStreamTrack>>& aTrack,
             CallerType aCallerType, ErrorResult& aRv);

  void Stop();

  void Abort();

  IMPL_EVENT_HANDLER(audiostart)
  IMPL_EVENT_HANDLER(soundstart)
  IMPL_EVENT_HANDLER(speechstart)
  IMPL_EVENT_HANDLER(speechend)
  IMPL_EVENT_HANDLER(soundend)
  IMPL_EVENT_HANDLER(audioend)
  IMPL_EVENT_HANDLER(result)
  IMPL_EVENT_HANDLER(nomatch)
  IMPL_EVENT_HANDLER(error)
  IMPL_EVENT_HANDLER(start)
  IMPL_EVENT_HANDLER(end)

  void NotifyTrackAdded(const RefPtr<MediaStreamTrack>& aTrack);

  class TrackListener final : public DOMMediaStream::TrackListener {
   public:
    NS_DECL_ISUPPORTS_INHERITED
    NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(TrackListener,
                                             DOMMediaStream::TrackListener)
    explicit TrackListener(SpeechRecognition* aSpeechRecognition)
        : mSpeechRecognition(aSpeechRecognition) {}
    void NotifyTrackAdded(const RefPtr<MediaStreamTrack>& aTrack) override {
      mSpeechRecognition->NotifyTrackAdded(aTrack);
    }

   private:
    virtual ~TrackListener() = default;
    RefPtr<SpeechRecognition> mSpeechRecognition;
  };

  // aMessage should be valid UTF-8, but invalid UTF-8 byte sequences are
  // replaced with the REPLACEMENT CHARACTER on conversion to UTF-16.
  void DispatchError(SpeechRecognitionErrorCode aErrorCode,
                     const nsACString& aMessage);
  template <int N>
  void DispatchError(SpeechRecognitionErrorCode aErrorCode,
                     const char (&aMessage)[N]) {
    DispatchError(aErrorCode, nsLiteralCString(aMessage));
  }
  // Backend methods
  void HandleRecognitionResultFromBackend(const nsCString& aTranscript,
                                          bool aIsFinal);
  void HandleRecognitionErrorFromBackend(const nsCString& aError);
  // Called once the backend's session is initialized and ready to receive
  // audio; combined with a track being attached (mTrack), this determines
  // when "start" fires (see MaybeDispatchStart()).
  void NotifyBackendListening();

  // A backend's callbacks are bound to that specific instance and can still
  // be in flight when it's superseded by a newer one (e.g. stop() followed
  // immediately by start()). DispatchToParentIfAlive uses this to drop
  // notifications from a backend that is no longer the current one, rather
  // than misattributing them to whatever session happens to be active by the
  // time the callback reaches the main thread.
  bool IsCurrentBackend(const SpeechRecognitionBackend* aBackend) const {
    return mBackend == aBackend;
  }

 private:
  virtual ~SpeechRecognition();

  NS_IMETHOD StartRecording(RefPtr<AudioStreamTrack>& aDOMStream);
  RefPtr<GenericNonExclusivePromise> StopRecording();

  void Reset();
  void ResetAndEnd();
  // Fires "start" once the system is successfully listening: the backend
  // session is initialized and a live track is attached (mTrack).
  void MaybeDispatchStart();

  RefPtr<DOMMediaStream> mStream;
  RefPtr<AudioStreamTrack> mTrack;
  bool mTrackIsOwned = false;
  RefPtr<GenericNonExclusivePromise> mStopRecordingPromise;
  RefPtr<SpeechTrackListener> mSpeechListener;

  // Tracks if recognition has been started (spec's [[started]] internal slot)
  bool mStarted;
  // Whether the backend has reported its session as initialized. See
  // MaybeDispatchStart().
  bool mBackendListening = false;
  // Whether "start" has already been dispatched for the current session.
  bool mStartDispatched = false;

  nsString mLang;

  RefPtr<SpeechGrammarList> mSpeechGrammarList;

  bool mContinuous;
  bool mInterimResults;
  uint32_t mMaxAlternatives;
  bool mProcessLocally = false;
  // The backend gets these at Start() time; spec is unclear on dynamic updates
  // Probably better as a SimpleMap or something so it's sparse
  // https://github.com/WebAudio/web-speech-api/issues/172
  nsTArray<RefPtr<SpeechRecognitionPhrase>> mPhrases;
  RefPtr<TrackListener> mListener;
  // Backend instance for handling audio processing
  RefPtr<SpeechRecognitionBackend> mBackend;

  static nsTHashSet<nsCString> sDownloadingLanguages;
};

}  // namespace dom

inline nsISupports* ToSupports(dom::SpeechRecognition* aRec) {
  return ToSupports(static_cast<DOMEventTargetHelper*>(aRec));
}

}  // namespace mozilla

#endif // DOM_MEDIA_WEBSPEECH_RECOGNITION_SPEECHRECOGNITION_H_
