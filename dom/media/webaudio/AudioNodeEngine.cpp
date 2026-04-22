/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AudioNodeEngine.h"

#include "mozilla/AbstractThread.h"
#ifdef USE_NEON
#  include "AudioNodeEngineGeneric.h"
#  include "mozilla/arm.h"
#endif
#ifdef USE_SSE2
#  include "AudioNodeEngineGeneric.h"
#  include "mozilla/SSE.h"
#endif
#if defined(USE_SSE42) && defined(USE_FMA3)
#  include "AudioNodeEngineGeneric.h"
#  include "mozilla/SSE.h"
#endif
#include "AudioBlock.h"
#include "Tracing.h"

namespace mozilla {

already_AddRefed<ThreadSharedFloatArrayBufferList>
ThreadSharedFloatArrayBufferList::Create(uint32_t aChannelCount, size_t aLength,
                                         const mozilla::fallible_t&) {
  RefPtr<ThreadSharedFloatArrayBufferList> buffer =
      new ThreadSharedFloatArrayBufferList(aChannelCount);

  for (uint32_t i = 0; i < aChannelCount; ++i) {
    float* channelData = js_pod_malloc<float>(aLength);
    if (!channelData) {
      return nullptr;
    }

    buffer->SetData(i, channelData, js_free, channelData);
  }

  return buffer.forget();
}

void WriteZeroesToAudioBlock(AudioBlock* aChunk, uint32_t aStart,
                             uint32_t aLength) {
  MOZ_ASSERT(aStart + aLength <= aChunk->GetDuration());
  MOZ_ASSERT(!aChunk->IsNull(), "You should pass a non-null chunk");
  if (aLength == 0) {
    return;
  }

  for (uint32_t i = 0; i < aChunk->ChannelCount(); ++i) {
    PodZero(aChunk->ChannelFloatsForWrite(i) + aStart, aLength);
  }
}

void AudioBufferCopyWithScale(const float* aInput, float aScale, float* aOutput,
                              uint32_t aSize) {
  if (aScale == 1.0f) {
    PodCopy(aOutput, aInput, aSize);
  } else {
    for (uint32_t i = 0; i < aSize; ++i) {
      aOutput[i] = aInput[i] * aScale;
    }
  }
}

void AudioBufferAddWithScale(const float* aInput, float aScale, float* aOutput,
                             uint32_t aSize) {
#ifdef USE_NEON
  if (mozilla::supports_neon()) {
    Engine<xsimd::neon>::AudioBufferAddWithScale(aInput, aScale, aOutput,
                                                 aSize);
    return;
  }
#endif

#ifdef USE_SSE2
  if (mozilla::supports_sse2()) {
#  if defined(USE_SSE42) && defined(USE_FMA3)
    if (mozilla::supports_fma3() && mozilla::supports_sse4_2()) {
      Engine<xsimd::fma3<xsimd::sse4_2>>::AudioBufferAddWithScale(
          aInput, aScale, aOutput, aSize);
    } else
#  endif
    {
      Engine<xsimd::sse2>::AudioBufferAddWithScale(aInput, aScale, aOutput,
                                                   aSize);
    }
    return;
  }
#endif

  if (aScale == 1.0f) {
    for (uint32_t i = 0; i < aSize; ++i) {
      aOutput[i] += aInput[i];
    }
  } else {
    for (uint32_t i = 0; i < aSize; ++i) {
      aOutput[i] += aInput[i] * aScale;
    }
  }
}

void AudioBufferCopyChannelWithScale(const float* aInput, float aScale,
                                    float* aOutput, uint32_t aSize) {
  if (aScale == 1.0f) {
    memcpy(aOutput, aInput, aSize * sizeof(float));
  } else {
#ifdef USE_NEON
    if (mozilla::supports_neon()) {
      Engine<xsimd::neon>::AudioBufferCopyChannelWithScale(aInput, aScale,
                                                          aOutput, aSize);
      return;
    }
#endif

#ifdef USE_SSE2
    if (mozilla::supports_sse2()) {
      Engine<xsimd::sse2>::AudioBufferCopyChannelWithScale(aInput, aScale,
                                                          aOutput, aSize);
      return;
    }
#endif

    for (uint32_t i = 0; i < aSize; ++i) {
      aOutput[i] = aInput[i] * aScale;
    }
  }
}

void BufferComplexMultiply(const float* aInput, const float* aScale,
                           float* aOutput, uint32_t aSize) {
#ifdef USE_NEON
  if (mozilla::supports_neon()) {
    Engine<xsimd::neon>::BufferComplexMultiply(aInput, aScale, aOutput, aSize);
    return;
  }
#endif
#ifdef USE_SSE2
  if (mozilla::supports_sse()) {
#  if defined(USE_SSE42) && defined(USE_FMA3)
    if (mozilla::supports_fma3() && mozilla::supports_sse4_2()) {
      Engine<xsimd::fma3<xsimd::sse4_2>>::BufferComplexMultiply(aInput, aScale,
                                                                aOutput, aSize);
    } else
#  endif
    {
      Engine<xsimd::sse2>::BufferComplexMultiply(aInput, aScale, aOutput,
                                                 aSize);
    }
    return;
  }
#endif

  for (uint32_t i = 0; i < aSize * 2; i += 2) {
    float real1 = aInput[i];
    float imag1 = aInput[i + 1];
    float real2 = aScale[i];
    float imag2 = aScale[i + 1];
    float realResult = real1 * real2 - imag1 * imag2;
    float imagResult = real1 * imag2 + imag1 * real2;
    aOutput[i] = realResult;
    aOutput[i + 1] = imagResult;
  }
}

float AudioBufferPeakValue(const float* aInput, uint32_t aSize) {
  float max = 0.0f;
  for (uint32_t i = 0; i < aSize; i++) {
    float mag = fabs(aInput[i]);
    if (mag > max) {
      max = mag;
    }
  }
  return max;
}

void AudioBufferCopyChannelWithScale(const float* aInput, const float* aScale,
                                    float* aOutput, uint32_t aSize) {
#ifdef USE_NEON
  if (mozilla::supports_neon()) {
    Engine<xsimd::neon>::AudioBufferCopyChannelWithScale(aInput, aScale, aOutput,
                                                        aSize);
    return;
  }
#endif

#ifdef USE_SSE2
  if (mozilla::supports_sse2()) {
    Engine<xsimd::sse2>::AudioBufferCopyChannelWithScale(aInput, aScale, aOutput,
                                                        aSize);
    return;
  }
#endif

  for (uint32_t i = 0; i < aSize; ++i) {
    aOutput[i] = aInput[i] * aScale[i];
  }
}

void AudioBufferInPlaceScale(float* aBlock, float aScale, uint32_t aSize) {
  if (aScale == 1.0f) {
    return;
  }
#ifdef USE_NEON
  if (mozilla::supports_neon()) {
    Engine<xsimd::neon>::AudioBufferInPlaceScale(aBlock, aScale, aSize);
    return;
  }
#endif

#ifdef USE_SSE2
  if (mozilla::supports_sse2()) {
    Engine<xsimd::sse2>::AudioBufferInPlaceScale(aBlock, aScale, aSize);
    return;
  }
#endif

  for (uint32_t i = 0; i < aSize; ++i) {
    *aBlock++ *= aScale;
  }
}

void AudioBufferInPlaceScale(float* aBlock, float* aScale, uint32_t aSize) {
#ifdef USE_NEON
  if (mozilla::supports_neon()) {
    Engine<xsimd::neon>::AudioBufferInPlaceScale(aBlock, aScale, aSize);
    return;
  }
#endif

#ifdef USE_SSE2
  if (mozilla::supports_sse2()) {
    Engine<xsimd::sse2>::AudioBufferInPlaceScale(aBlock, aScale, aSize);
    return;
  }
#endif

  for (uint32_t i = 0; i < aSize; ++i) {
    *aBlock++ *= *aScale++;
  }
}

void AudioBufferPanMonoToStereo(const float* aInput, float* aGainL,
                               float* aGainR, float* aOutputL, float* aOutputR,
                               uint32_t aSize) {
  AudioBufferCopyChannelWithScale(aInput, aGainL, aOutputL, aSize);
  AudioBufferCopyChannelWithScale(aInput, aGainR, aOutputR, aSize);
}

void AudioBufferPanMonoToStereo(const float* aInput, float aGainL, float aGainR,
                               float* aOutputL, float* aOutputR,
                               uint32_t aSize) {
  AudioBufferCopyChannelWithScale(aInput, aGainL, aOutputL, aSize);
  AudioBufferCopyChannelWithScale(aInput, aGainR, aOutputR, aSize);
}

void AudioBufferPanStereoToStereo(const float* aInputL, const float* aInputR,
                                 float aGainL, float aGainR, bool aIsOnTheLeft,
                                 float* aOutputL, float* aOutputR,
                                 uint32_t aSize) {
#ifdef USE_NEON
  if (mozilla::supports_neon()) {