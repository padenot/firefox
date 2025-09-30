# Web Speech API Recognition Update Plan

## Current State vs. Specification Differences

### 1. SpeechRecognition Interface

#### Missing Properties/Methods in Firefox:
- **`processLocally`** attribute - Forces local processing of speech recognition
- **`phrases`** attribute - Array of phrases for contextual biasing
- **`available()`** method - Checks if speech recognition is available
- **`install()`** method - Attempts to install language recognition packs
- **`start(audioTrack)`** overload - Accepts MediaStreamTrack instead of MediaStream

#### Deprecated but still present:
- **`serviceURI`** attribute - Firefox-specific, not in current spec

### 2. Event Handling
Current Firefox implementation appears complete for events.

### 3. SpeechGrammar and SpeechGrammarList
These are deprecated in the spec but still present in Firefox - marked for backwards compatibility only.

## Implementation Tasks

### Phase 1: WebIDL Updates

1. **SpeechRecognition.webidl**
   - Add `processLocally` boolean attribute
   - Add `phrases` sequence<DOMString> attribute
   - Add `available()` static method returning Promise<boolean>
   - Add `install()` static method returning Promise<undefined>
   - Add overload for `start()` accepting MediaStreamTrack
   - Mark `serviceURI` as deprecated (keep for compatibility)

### Phase 2: Implementation Stubs

1. **SpeechRecognition.h/.cpp**
   - Add member variables for new properties
   - Implement stub methods that return appropriate defaults
   - Add TODO comments for actual implementation

2. **Integration with HWInference utility process**
   - Plan integration points for local processing
   - Consider using HWInference for `processLocally` feature

### Phase 3: Future Work (Out of Scope Now)

1. Actual implementation of:
   - Local speech recognition using HWInference utility
   - Contextual biasing with phrases
   - Language pack installation
   - Availability checking

## Compatibility Considerations

1. Keep `serviceURI` for backwards compatibility but mark deprecated
2. Keep SpeechGrammar/SpeechGrammarList interfaces as no-ops
3. Ensure webkit prefixed constructors continue to work

## Testing Requirements

1. Update existing tests to handle new properties
2. Add tests for new methods (even if stubbed)
3. Ensure backwards compatibility tests pass

## Security Considerations

1. `processLocally` should respect privacy preferences
2. `install()` should require user permission
3. Audio track access should follow existing permissions model

## Implementation Status

### Completed (with stubs):
✅ Added `processLocally` attribute - Controls local vs remote processing
✅ Added `phrases` attribute - For contextual biasing (cached/frozen)
✅ Added `available()` static method - Returns Promise<boolean>
✅ Added `install()` static method - Returns Promise<undefined>
✅ Added preference for deprecated `serviceURI` attribute
✅ All new features compile and build successfully

### TODO - Future Implementation:
- [ ] Integrate `processLocally` with HWInference utility process for local recognition
- [ ] Implement actual availability checking in `available()`
- [ ] Implement language pack installation in `install()`
- [ ] Add support for MediaStreamTrack overload in `start()`
- [ ] Implement contextual biasing using `phrases`
- [ ] Connect to actual speech recognition backend (local or remote)

### Files Modified:
- `dom/webidl/SpeechRecognition.webidl` - Updated to current spec
- `dom/media/webspeech/recognition/SpeechRecognition.h` - Added new method declarations
- `dom/media/webspeech/recognition/SpeechRecognition.cpp` - Added stub implementations
- `modules/libpref/init/StaticPrefList.yaml` - Added serviceURI preference

All implementations are properly stubbed with clear TODO comments for future work.