/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechRecognition.h"

#include <algorithm>

#include "AudioSegment.h"
#include "CubebUtils.h"
#include "MainThreadUtils.h"
#include "MediaEnginePrefs.h"
#include "SpeechRecognitionAlternative.h"
#include "SpeechRecognitionBackend.h"
#include "SpeechRecognitionPermissionRequest.h"
#include "SpeechRecognitionResult.h"
#include "SpeechRecognitionResultList.h"
#include "SpeechTrackListener.h"
#include "VideoUtils.h"
#include "js/Value.h"
#include "mozilla/AbstractThread.h"
#include "mozilla/ErrorNames.h"
#include "mozilla/MediaManager.h"
#include "mozilla/Preferences.h"
#include "mozilla/dom/AudioStreamTrack.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/BrowsingContext.h"
#include "mozilla/dom/DOMException.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/Event.h"
#include "mozilla/dom/FeaturePolicyUtils.h"
#include "mozilla/dom/MediaStreamBinding.h"
#include "mozilla/dom/MediaStreamError.h"
#include "mozilla/dom/MediaStreamTrackBinding.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "mozilla/dom/RootedDictionary.h"
#include "mozilla/dom/ScriptSettings.h"
#include "mozilla/dom/SpeechGrammar.h"
#include "mozilla/dom/SpeechRecognitionErrorEvent.h"
#include "mozilla/dom/SpeechRecognitionEvent.h"
#include "mozilla/dom/SpeechRecognitionPhrase.h"
#include "mozilla/dom/ToJSValue.h"
#include "mozilla/intl/Locale.h"
#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsContentUtils.h"
#include "nsCycleCollectionParticipant.h"
#include "nsGkAtoms.h"
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

namespace mozilla {
class Promise;
};

namespace mozilla::dom {

// Returns true when the user has blocked AI features globally
// (browser.ai.control.default == "blocked") or blocked speech recognition
// specifically (browser.ai.control.speechRecognition == "blocked").
static bool IsSpeechRecognitionAIBlocked() {
  nsAutoCString defaultControl;
  Preferences::GetCString("browser.ai.control.default", defaultControl);
  if (defaultControl.EqualsLiteral("blocked")) {
    return true;
  }
  nsAutoCString speechControl;
  Preferences::GetCString("browser.ai.control.speechRecognition",
                          speechControl);
  return speechControl.EqualsLiteral("blocked");
}

static LazyLogModule gSpeechRecognitionLog("SpeechRecognition");

#define LOG(...) \
  MOZ_LOG_FMT(gSpeechRecognitionLog, LogLevel::Debug, __VA_ARGS__)
#define LOGV(...) \
  MOZ_LOG_FMT(gSpeechRecognitionLog, LogLevel::Verbose, __VA_ARGS__)
#define LOGE(...) \
  MOZ_LOG_FMT(gSpeechRecognitionLog, LogLevel::Error, __VA_ARGS__)

NS_IMPL_CYCLE_COLLECTION_CLASS(SpeechRecognition)
NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(SpeechRecognition,
                                                DOMEventTargetHelper)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mTrack, mSpeechGrammarList, mListener,
                                  mPhrases)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_WEAK_PTR
NS_IMPL_CYCLE_COLLECTION_UNLINK_END
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(SpeechRecognition,
                                                  DOMEventTargetHelper)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mTrack, mSpeechGrammarList, mListener,
                                    mPhrases)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

nsTHashSet<nsCString> SpeechRecognition::sDownloadingLanguages;

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(SpeechRecognition)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)
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

// Lifetime considerations:
// This class, like other classes interacting with MediaStreams, has a
// non-standard lifetime:
// - If script has a direct ref, SpeechRecognition stays alive, by definition.
// Depending on its state, the backend can be cleared / destroyed early or not.
// - Otherwise, if the input track's readyState is "live" and there is some
// callback registered, SpeechRecognition stays alive. Otherwise, e.g. if the
// input track is live, script has no refs, and there are no callbacks, the
// instance isn't useful and can be collected.
//
// This is implemented using the KeepAliveIfHasListenersFor mechanism from
// DOMEventTargetHelper. When recognition starts (mStarted becomes true), we
// register the relevant event types that should keep this object alive if
// listeners are present. When recognition ends (Reset is called -- directly or
// indirectly), we unregister them.
static constexpr nsStaticAtom* const kKeepAliveEventTypes[] = {
    nsGkAtoms::onstart,       nsGkAtoms::onaudiostart, nsGkAtoms::onsoundstart,
    nsGkAtoms::onspeechstart, nsGkAtoms::onspeechend,  nsGkAtoms::onsoundend,
    nsGkAtoms::onaudioend,    nsGkAtoms::onresult,     nsGkAtoms::onnomatch,
    nsGkAtoms::onerror,       nsGkAtoms::onend};

SpeechRecognition::SpeechRecognition(nsPIDOMWindowInner* aOwnerWindow)
    : DOMEventTargetHelper(aOwnerWindow),
      mStarted(false),
      mSpeechGrammarList(new SpeechGrammarList(aOwnerWindow)),
      mContinuous(false),
      mInterimResults(false),
      mMaxAlternatives(1) {
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

void SpeechRecognition::DisconnectFromOwner() {
  AssertIsOnMainThread();
  if (mBackend) {
    mBackend->Abort();
    mBackend = nullptr;
  }
  Reset();
  DOMEventTargetHelper::DisconnectFromOwner();
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
  if (mStarted) {
    for (nsStaticAtom* atom : kKeepAliveEventTypes) {
      IgnoreKeepAliveIfHasListenersFor(atom);
    }
  }
  mStarted = false;
  mTrack = nullptr;
  mStopRecordingPromise = nullptr;
}

void SpeechRecognition::ResetAndEnd() {
  Reset();
  DispatchTrustedEvent(u"end"_ns);
}

NS_IMETHODIMP
SpeechRecognition::StartRecording(RefPtr<AudioStreamTrack>& aTrack) {
  AssertIsOnMainThread();
  MOZ_ASSERT(!aTrack->Ended());
  MOZ_ASSERT(mBackend);

  mTrack = aTrack;
  mBackend->AttachToTrack(aTrack);

  return NS_OK;
}

RefPtr<GenericNonExclusivePromise> SpeechRecognition::StopRecording() {
  AssertIsOnMainThread();
  if (!mTrack) {
    // Recording wasn't started, or has already been stopped.
    return GenericNonExclusivePromise::CreateAndResolve(true, __func__);
  }

  if (mStopRecordingPromise) {
    return mStopRecordingPromise;
  }

  if (mBackend) {
    mBackend->DetachFromTrack();
  }

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
  Span<const char> langSpan(utf8Lang.get(), utf8Lang.Length());

  // Empty strings are not valid BCP47 language tags
  if (langSpan.IsEmpty()) {
    aRv.ThrowSyntaxError("Invalid BCP47 language tag");
    return false;
  }

  intl::Locale locale;
  auto result = intl::LocaleParser::TryParse(langSpan, locale);

  if (result.isErr()) {
    aRv.ThrowSyntaxError("Invalid BCP47 language tag");
    return false;
  }

  return true;
}

bool SpeechRecognition::ProcessLocally() const { return mProcessLocally; }

void SpeechRecognition::SetProcessLocally(bool aProcessLocally) {
  mProcessLocally = aProcessLocally;
}

bool SpeechRecognition::UnspokenPunctuation() const {
  return mUnspokenPunctuation;
}

void SpeechRecognition::SetUnspokenPunctuation(bool aUnspokenPunctuation) {
  mUnspokenPunctuation = aUnspokenPunctuation;
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

// https://webaudio.github.io/web-speech-api/#dom-speechrecognition-available
// Runs the availability algorithm:
// https://webaudio.github.io/web-speech-api/#availability-algorithm
/* static */
already_AddRefed<Promise> SpeechRecognition::Available(
    const GlobalObject& aGlobal, const SpeechRecognitionOptions& aOptions,
    ErrorResult& aRv) {
  AssertIsOnMainThread();

  // Step 1: Check if Document is fully active.
  nsCOMPtr<nsPIDOMWindowInner> window =
      do_QueryInterface(aGlobal.GetAsSupports());
  if (!window || !window->IsFullyActive()) {
    aRv.ThrowInvalidStateError("The document is not fully active.");
    return nullptr;
  }

  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());
  if (!global) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  // Step 3: Validate all language tags are valid BCP47.
  for (const nsString& lang : aOptions.mLangs) {
    if (!ValidateBCP47Language(lang, aRv)) {
      return nullptr;
    }
  }

  RefPtr<Promise> promise = Promise::Create(global, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  // available() is gated behind the "on-device-speech-recognition"
  // policy-controlled feature (default allowlist 'self'). When it is
  // disallowed (e.g. a cross-origin iframe or an explicit 'none' policy),
  // report unavailable.
  if (nsCOMPtr<Document> doc = window->GetExtantDoc();
      !doc || !FeaturePolicyUtils::IsFeatureAllowed(
                  doc, u"on-device-speech-recognition"_ns)) {
    promise->MaybeResolve(AvailabilityStatus::Unavailable);
    return promise.forget();
  }

  // Step 4: If AI controls are blocked, report as unavailable.
  if (IsSpeechRecognitionAIBlocked()) {
    promise->MaybeResolve(AvailabilityStatus::Unavailable);
    return promise.forget();
  }

  // Step 5: If processLocally is false, Gecko doesn't support remote
  // recognition.
  if (!aOptions.mProcessLocally) {
    promise->MaybeResolve(AvailabilityStatus::Unavailable);
    return promise.forget();
  }

  // Step 6: processLocally is true.
  // If langs is empty, return unavailable.
  if (aOptions.mLangs.IsEmpty()) {
    promise->MaybeResolve(AvailabilityStatus::Unavailable);
    return promise.forget();
  }

  // Check if any requested language is currently being downloaded.
  // https://bugzilla.mozilla.org/show_bug.cgi?id=2006385
  // The spec requires per-language status checking with a
  // "worst status" algorithm. This is best implemented in the backend.
  for (const nsString& lang : aOptions.mLangs) {
    if (sDownloadingLanguages.Contains(NS_ConvertUTF16toUTF8(lang))) {
      promise->MaybeResolve(AvailabilityStatus::Downloading);
      return promise.forget();
    }
  }

  nsTArray<nsString> languages;
  for (const nsString& lang : aOptions.mLangs) {
    languages.AppendElement(lang);
  }

  return SpeechRecognitionBackend::Available(global, languages);
}

class InstallCompletionHandler final : public PromiseNativeHandler {
 public:
  NS_DECL_ISUPPORTS

  explicit InstallCompletionHandler(nsTArray<nsCString>&& aLanguages)
      : mLanguages(std::move(aLanguages)) {}

  void ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                        ErrorResult& aRv) override {
    Cleanup();
  }

  void RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                        ErrorResult& aRv) override {
    Cleanup();
  }

 private:
  ~InstallCompletionHandler() = default;

  void Cleanup() {
    AssertIsOnMainThread();
    for (const nsCString& lang : mLanguages) {
      SpeechRecognition::RemoveDownloadingLanguage(lang);
    }
  }

  nsTArray<nsCString> mLanguages;
};

NS_IMPL_ISUPPORTS0(InstallCompletionHandler)

// Receives the model download size (computed in the utility process) and shows
// the download permission prompt with it. Created by InstallGateHandler once a
// download is actually required.
class SpeechModelSizeHandler final : public PromiseNativeHandler {
 public:
  NS_DECL_ISUPPORTS

  SpeechModelSizeHandler(nsPIDOMWindowInner* aWindow, Promise* aInstallPromise,
                         nsTArray<nsString>&& aLanguages)
      : mWindow(aWindow),
        mInstallPromise(aInstallPromise),
        mLanguages(std::move(aLanguages)) {}

  void ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                        ErrorResult& aRv) override {
    uint32_t sizeMB =
        aValue.isNumber() ? static_cast<uint32_t>(aValue.toNumber()) : 0;
    Prompt(sizeMB);
  }

  void RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                        ErrorResult& aRv) override {
    Prompt(0);
  }

 private:
  ~SpeechModelSizeHandler() = default;

  void Prompt(uint32_t aSizeMB) {
    AssertIsOnMainThread();
    auto permRequest = MakeRefPtr<SpeechRecognitionPermissionRequest>(
        mWindow, mInstallPromise, mLanguages, aSizeMB);
    NS_DispatchToMainThread(permRequest.forget());
  }

  RefPtr<nsPIDOMWindowInner> mWindow;
  RefPtr<Promise> mInstallPromise;
  nsTArray<nsString> mLanguages;
};

NS_IMPL_ISUPPORTS0(SpeechModelSizeHandler)

// Receives the result of Available() inside Install(). If the model is already
// present, resolves the outer install promise immediately without prompting.
// Otherwise fetches the download size and shows the permission prompt.
class InstallGateHandler final : public PromiseNativeHandler {
 public:
  NS_DECL_ISUPPORTS

  InstallGateHandler(nsPIDOMWindowInner* aWindow, Promise* aInstallPromise,
                     nsTArray<nsString>&& aLanguages,
                     nsTArray<nsCString>&& aLanguagesUtf8)
      : mWindow(aWindow),
        mInstallPromise(aInstallPromise),
        mLanguages(std::move(aLanguages)),
        mLanguagesUtf8(std::move(aLanguagesUtf8)) {}

  void ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                        ErrorResult& aRv) override {
    AssertIsOnMainThread();
    // aValue is the AvailabilityStatus string ("available", "downloadable", …).
    nsAutoJSString statusStr;
    if (aValue.isString() && statusStr.init(aCx, aValue.toString()) &&
        statusStr.EqualsLiteral("available")) {
      mInstallPromise->MaybeResolve(true);
      return;
    }
    // Re-check downloading (state may have changed while Available() was in
    // flight).
    for (const nsCString& lang : mLanguagesUtf8) {
      if (SpeechRecognition::IsLanguageDownloading(lang)) {
        mInstallPromise->MaybeResolve(false);
        return;
      }
    }
    // Fetch the model download size (computed in the utility process, which
    // owns the model table) before prompting, then show the prompt with it.
    nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(mWindow);
    RefPtr<Promise> sizePromise =
        SpeechRecognitionBackend::GetModelDownloadSize(global, mLanguages);
    auto sizeHandler = MakeRefPtr<SpeechModelSizeHandler>(
        mWindow, mInstallPromise, std::move(mLanguages));
    if (!sizePromise) {
      sizeHandler->RejectedCallback(aCx, JS::UndefinedHandleValue, aRv);
      return;
    }
    sizePromise->AppendNativeHandler(sizeHandler);
  }

  void RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue,
                        ErrorResult& aRv) override {
    mInstallPromise->MaybeResolve(false);
  }

 private:
  ~InstallGateHandler() = default;

  RefPtr<nsPIDOMWindowInner> mWindow;
  RefPtr<Promise> mInstallPromise;
  nsTArray<nsString> mLanguages;
  nsTArray<nsCString> mLanguagesUtf8;
};

NS_IMPL_ISUPPORTS0(InstallGateHandler)

/* static */
void SpeechRecognition::AddDownloadingLanguage(const nsCString& aLanguage) {
  AssertIsOnMainThread();
  sDownloadingLanguages.Insert(aLanguage);
}

/* static */
void SpeechRecognition::RemoveDownloadingLanguage(const nsCString& aLanguage) {
  AssertIsOnMainThread();
  sDownloadingLanguages.Remove(aLanguage);
}

bool SpeechRecognition::IsLanguageDownloading(const nsCString& aLanguage) {
  AssertIsOnMainThread();
  return sDownloadingLanguages.Contains(aLanguage);
}

// https://webaudio.github.io/web-speech-api/#dom-speechrecognition-install
/* static */
already_AddRefed<Promise> SpeechRecognition::Install(
    const GlobalObject& aGlobal, const SpeechRecognitionOptions& aOptions,
    ErrorResult& aRv) {
  AssertIsOnMainThread();
  // Step 1: the document must be fully active.
  nsCOMPtr<nsPIDOMWindowInner> window =
      do_QueryInterface(aGlobal.GetAsSupports());
  nsCOMPtr<Document> doc = window ? window->GetExtantDoc() : nullptr;
  if (!window || !window->IsFullyActive() || !doc) {
    // `this` may belong to a now-detached frame. Gecko drops promise reaction
    // jobs whose realm is dead, so a promise rejected in the frame's (dead)
    // realm would never settle for the caller. Create the rejection in the
    // caller's (entry) realm so it settles, but build the DOMException in the
    // frame's realm so cross-realm `instanceof` checks still see the frame's
    // exception.
    nsCOMPtr<nsIGlobalObject> entry = GetEntryGlobal();
    nsCOMPtr<nsIGlobalObject> frameGlobal =
        do_QueryInterface(aGlobal.GetAsSupports());
    JSObject* frameJSGlobal =
        frameGlobal ? frameGlobal->GetGlobalJSObject() : nullptr;
    if (!entry || !frameJSGlobal) {
      aRv.ThrowInvalidStateError("The document is not fully active.");
      return nullptr;
    }
    JSContext* cx = aGlobal.Context();
    JS::Rooted<JS::Value> error(cx);
    {
      JSAutoRealm ar(cx, frameJSGlobal);
      RefPtr<DOMException> exception =
          DOMException::Create(NS_ERROR_DOM_INVALID_STATE_ERR,
                               "The document is not fully active."_ns);
      if (!ToJSValue(cx, exception, &error)) {
        JS_ClearPendingException(cx);
        aRv.ThrowInvalidStateError("The document is not fully active.");
        return nullptr;
      }
    }
    RefPtr<Promise> promise = Promise::Create(entry, aRv);
    if (aRv.Failed()) {
      return nullptr;
    }
    promise->MaybeReject(error);
    return promise.forget();
  }

  // install() is gated behind the "on-device-speech-recognition"
  // policy-controlled feature (default allowlist 'self'). When it is
  // disallowed, reject with NotAllowedError, using a cross-origin-specific
  // message when the document is a cross-origin subframe.
  if (!FeaturePolicyUtils::IsFeatureAllowed(
          doc, u"on-device-speech-recognition"_ns)) {
    BrowsingContext* bc = doc->GetBrowsingContext();
    if (bc && !bc->SameOriginWithTop()) {
      aRv.ThrowNotAllowedError(
          "install() is not allowed in a cross-origin iframe");
    } else {
      aRv.ThrowNotAllowedError(
          "install() is not allowed by the on-device-speech-recognition "
          "permissions policy");
    }
    return nullptr;
  }

  // install() initiates a potentially large download, so it requires transient
  // user activation. The testing pref bypasses this so automated tests can
  // call install() without a synthetic user gesture.
  if (!Preferences::GetBool(
          "media.webspeech.recognition.model-download.prompt.testing", false) &&
      !doc->HasValidTransientUserGestureActivation()) {
    aRv.ThrowNotAllowedError("install() requires transient user activation");
    return nullptr;
  }

  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(aGlobal.GetAsSupports());
  if (!global) {
    aRv.Throw(NS_ERROR_FAILURE);
    return nullptr;
  }

  RefPtr<Promise> promise = Promise::Create(global, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  if (IsSpeechRecognitionAIBlocked()) {
    promise->MaybeResolve(false);
    return promise.forget();
  }

  // Step 3: install resolves false when langs is empty or any requested
  // language's on-device pack is unsupported. A tag we cannot parse as BCP47
  // is necessarily unsupported, so it resolves false rather than throwing.
  // (Per spec step 2 a strictly-invalid tag is a SyntaxError; the deployed Web
  // Speech behaviour, exercised by WPT, resolves false for such tags.)
  if (aOptions.mLangs.IsEmpty()) {
    promise->MaybeResolve(false);
    return promise.forget();
  }
  for (const nsString& lang : aOptions.mLangs) {
    IgnoredErrorResult validateRv;
    if (!ValidateBCP47Language(lang, validateRv)) {
      promise->MaybeResolve(false);
      return promise.forget();
    }
  }

  nsTArray<nsCString> languagesUtf8;
  for (const nsString& lang : aOptions.mLangs) {
    languagesUtf8.AppendElement(NS_ConvertUTF16toUTF8(lang));
  }

  // Check if any language is already downloading
  for (const nsCString& lang : languagesUtf8) {
    if (sDownloadingLanguages.Contains(lang)) {
      LOG("Install: language {} already downloading", lang.get());
      promise->MaybeResolve(false);
      return promise.forget();
    }
  }

  // Check availability first: if the model is already installed there is no
  // download and no permission prompt is needed. The gate handler resolves the
  // promise immediately in that case, or dispatches the permission request.
  nsTArray<nsString> languages(aOptions.mLangs.Elements(),
                               aOptions.mLangs.Length());
  RefPtr<Promise> availPromise =
      SpeechRecognitionBackend::Available(global, languages);
  if (!availPromise) {
    promise->MaybeResolve(false);
    return promise.forget();
  }
  auto gate = MakeRefPtr<InstallGateHandler>(
      window, promise, std::move(languages), std::move(languagesUtf8));
  availPromise->AppendNativeHandler(gate);

  return promise.forget();
}

void SpeechRecognition::Start(const Optional<NonNull<MediaStreamTrack>>& aTrack,
                              CallerType aCallerType, ErrorResult& aRv) {
  AssertIsOnMainThread();
  LOG("SpeechRecognition::Start called");

  nsPIDOMWindowInner* win = GetOwnerWindow();
  if (!win || !win->IsFullyActive()) {
    aRv.ThrowInvalidStateError("The document is not fully active.");
    return;
  }

  // Check if already started (spec's [[started]] internal slot)
  if (mStarted) {
    aRv.ThrowInvalidStateError("Recognition has already been started");
    return;
  }

  MOZ_ASSERT(!mListener);
  MOZ_ASSERT(!mBackend);

  uint32_t graphRate = 0;
  if (aTrack.WasPassed()) {
    graphRate = aTrack.Value().Graph()->GraphRate();
  } else {
    // If using the microphone, it is always at the preferred rate
    graphRate =
        CubebUtils::PreferredSampleRate(/* shouldResistFingerPrinting*/ false);
  }

  // init and start the backend
  // Extract phrase strings from our local copy of SpeechRecognitionPhrase
  // objects. The backend gets these at Start() time; the spec is unclear on
  // dynamic updates
  // https://github.com/WebAudio/web-speech-api/issues/172
  nsTArray<nsString> phrasesForBackend;
  for (const auto& phrase : mPhrases) {
    if (phrase) {
      nsString phraseStr;
      phrase->GetPhrase(phraseStr);
      phrasesForBackend.AppendElement(phraseStr);
    }
  }
  // Validate track if provided
  RefPtr<AudioStreamTrack> audioTrack;
  if (aTrack.WasPassed()) {
    RefPtr<MediaStreamTrack> track = &aTrack.Value();
    audioTrack = track->AsAudioStreamTrack();

    if (!audioTrack) {
      aRv.ThrowInvalidStateError("MediaStreamTrack must be an audio track");
      return;
    }

    if (audioTrack->Ended()) {
      aRv.ThrowInvalidStateError("MediaStreamTrack is ended");
      return;
    }
  }

  // Per spec: if lang is unset, default to the document root element's language
  nsString effectiveLang = mLang;
  if (effectiveLang.IsEmpty()) {
    if (nsCOMPtr<Document> doc = win->GetExtantDoc()) {
      if (Element* root = doc->GetRootElement()) {
        root->GetLang(effectiveLang);
      }
    }
  }

  mBackend = new SpeechRecognitionBackend(this, graphRate, effectiveLang,
                                          phrasesForBackend);
  nsresult rv = mBackend->Start();
  if (NS_FAILED(rv)) {
    LOGE("Failed to start backend: {} ({:x})", GetStaticErrorName(rv),
         static_cast<uint32_t>(rv));
    mBackend = nullptr;
    DispatchError(SpeechRecognitionErrorCode::Service_not_allowed,
                  "Local speech recognition is not available"_ns);
    return;
  }

  mStarted = true;

  // Register keep-alive event types. While recognition is active, if script
  // has listeners for these events, the object stays alive even without a
  // direct reference from script.
  for (nsStaticAtom* atom : kKeepAliveEventTypes) {
    KeepAliveIfHasListenersFor(atom);
  }

  DispatchTrustedEvent(u"start"_ns);

  // MediaStreamTrack (argument passed) vs. Microphone (no argument passed)
  if (audioTrack) {
    NotifyTrackAdded(audioTrack);
  } else {
    mListener = new TrackListener(this);

    MediaStreamConstraints constraints;
    constraints.mAudio.SetAsBoolean() = true;

    AutoNoJSAPI nojsapi;
    RefPtr<SpeechRecognition> self(this);
    MediaManager::Get()
        ->GetUserMedia(GetOwnerWindow(), constraints, aCallerType)
        ->Then(
            GetCurrentSerialEventTarget(), __func__,
            [this, self](RefPtr<DOMMediaStream>&& aStream) {
              nsTArray<RefPtr<AudioStreamTrack>> tracks;
              aStream->GetAudioTracks(tracks);
              if (!mStarted) {
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
            [this, self](RefPtr<MediaMgrError>&& error) {
              if (!mStarted) {
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
  AssertIsOnMainThread();
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
  AssertIsOnMainThread();
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

  RefPtr<SpeechRecognitionErrorEvent> srError =
      new SpeechRecognitionErrorEvent(nullptr, nullptr, nullptr);

  srError->InitSpeechRecognitionError(u"error"_ns, true, false, aErrorCode,
                                      aMessage);
  srError->SetTrusted(true);

  DispatchEvent(*srError);
}

void SpeechRecognition::DispatchTrustedEventWithTimestamp(
    const nsAString& aEventName, TimeStamp aTimeStamp) {
  RefPtr<Event> event = NS_NewDOMEvent(this, nullptr, nullptr);
  event->InitEvent(aEventName, false, false);
  if (!aTimeStamp.IsNull()) {
    event->WidgetEventPtr()->mTimeStamp = aTimeStamp;
  }
  event->SetTrusted(true);
  ErrorResult rv;
  DispatchEvent(*event, rv);
}

void SpeechRecognition::HandleRecognitionResultFromBackend(
    const nsCString& aTranscript, bool aIsFinal, float aConfidence,
    TimeStamp aEventTime) {
  MOZ_ASSERT(NS_IsMainThread(), "Must be called on main thread");
  LOG("HandleRecognitionResultFromBackend: {} (final={}, conf={})",
      aTranscript.get(), aIsFinal, aConfidence);

  // Check if still active
  if (!mBackend) {
    LOG("Ignoring result - backend is gone");
    return;
  }

  // Per spec: when interimResults is false, interim results must not be
  // returned
  if (!aIsFinal && !mInterimResults) {
    LOG("Ignoring interim result - interimResults is false");
    return;
  }

  // NOTE: We don't implement non-continuous mode (mContinuous=false) for now.
  // The spec semantics are unclear with modern local LLM-based recognition.
  // See https://github.com/WebAudio/web-speech-api/issues/176

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
  // Per-result confidence, aggregated by the backend from the model's per-word
  // confidences (mean). The spec leaves the exact aggregation engine-defined;
  // the legacy backend, which has no per-word scores, reports 1.0.
  alternative->mConfidence = aConfidence;

  result->mItems.AppendElement(alternative);

  result->SetFinal(aIsFinal);

  resultList->mItems.AppendElement(result);

  RootedDictionary<SpeechRecognitionEventInit> init(RootingCx());
  init.mBubbles = true;
  init.mCancelable = false;
  init.mResultIndex = 0;
  init.mResults = resultList;
  init.mInterpretation = JS::NullValue();

  RefPtr<SpeechRecognitionEvent> domEvent =
      SpeechRecognitionEvent::Constructor(this, u"result"_ns, init);
  domEvent->SetTrusted(true);
  if (!aEventTime.IsNull()) {
    domEvent->WidgetEventPtr()->mTimeStamp = aEventTime;
  }
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

  RefPtr<SpeechRecognitionErrorEvent> srError =
      new SpeechRecognitionErrorEvent(nullptr, nullptr, nullptr);

  // Map backend errors to appropriate error codes
  SpeechRecognitionErrorCode errorCode = SpeechRecognitionErrorCode::Network;
  if (aError.EqualsLiteral("concurrent-session")) {
    // Use service-not-allowed for concurrent session rejection
    errorCode = SpeechRecognitionErrorCode::Service_not_allowed;
  }

  srError->InitSpeechRecognitionError(u"error"_ns, true, false, errorCode,
                                      aError);
  srError->SetTrusted(true);

  LOG("Dispatching error DOM event: {}", aError.get());
  DispatchEvent(*srError);
}

}  // namespace mozilla::dom

#undef LOG
#undef LOGV
#undef LOGE
