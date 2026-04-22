/* this source code form is subject to the terms of the mozilla public
 * license, v. 2.0. if a copy of the mpl was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_AUDIONODEENGINEGENERICIMPL_H_
#define MOZILLA_AUDIONODEENGINEGENERICIMPL_H_

#include "AlignmentUtils.h"
#include "AudioNodeEngineGeneric.h"

#if defined(__GNUC__) && __GNUC__ > 7
#  define MOZ_PRAGMA(tokens) _Pragma(#tokens)
#  define MOZ_UNROLL(factor) MOZ_PRAGMA(GCC unroll factor)
#elif defined(__INTEL_COMPILER) || (defined(__clang__) && __clang_major__ > 3)
#  define MOZ_PRAGMA(tokens) _Pragma(#tokens)
#  define MOZ_UNROLL(factor) MOZ_PRAGMA(unroll factor)
#else
#  define MOZ_UNROLL(_)
#endif

namespace mozilla {

template <class Arch>
static bool is_aligned(const void* ptr) {
  return (reinterpret_cast<uintptr_t>(ptr) &
          ~(static_cast<uintptr_t>(Arch::alignment()) - 1)) ==
         reinterpret_cast<uintptr_t>(ptr);
};

template <class Arch>
void Engine<Arch>::AudioBufferAddWithScale(const float* aInput, float aScale,
                                           float* aOutput, uint32_t aSize) {
  if constexpr (Arch::requires_alignment()) {
    if (aScale == 1.0f) {
      while (!is_aligned<Arch>(aInput) || !is_aligned<Arch>(aOutput)) {
        if (!aSize) return;
        *aOutput += *aInput;
        ++aOutput;
        ++aInput;
        --aSize;
      }
    } else {
      while (!is_aligned<Arch>(aInput) || !is_aligned<Arch>(aOutput)) {
        if (!aSize) return;
        *aOutput += *aInput * aScale;
        ++aOutput;
        ++aInput;
        --aSize;
      }
    }
  }
  MOZ_ASSERT(is_aligned<Arch>(aInput), "aInput is aligned");
  MOZ_ASSERT(is_aligned<Arch>(aOutput), "aOutput is aligned");

  xsimd::batch<float, Arch> vgain(aScale);

  uint32_t aVSize = aSize & ~(xsimd::batch<float, Arch>::size - 1);
  MOZ_UNROLL(4)
  for (unsigned i = 0; i < aVSize; i += xsimd::batch<float, Arch>::size) {
    auto vin1 = xsimd::batch<float, Arch>::load_aligned(&aInput[i]);
    auto vin2 = xsimd::batch<float, Arch>::load_aligned(&aOutput[i]);
    auto vout = xsimd::fma(vin1, vgain, vin2);
    vout.store_aligned(&aOutput[i]);
  }

  for (unsigned i = aVSize; i < aSize; ++i) {
    aOutput[i] += aInput[i] * aScale;
  }
}

template <class Arch>
void Engine<Arch>::AudioBufferCopyChannelWithScale(const float* aInput,
                                                  float aScale, float* aOutput,
                                                  uint32_t aSize) {
  MOZ_ASSERT(is_aligned<Arch>(aInput), "aInput is aligned");
  MOZ_ASSERT(is_aligned<Arch>(aOutput), "aOutput is aligned");
  MOZ_ASSERT((aSize % xsimd::batch<float, Arch>::size == 0),
             "requires tail processing");

  xsimd::batch<float, Arch> vgain = (aScale);

  MOZ_UNROLL(4)
  for (unsigned i = 0; i < aSize; i += xsimd::batch<float, Arch>::size) {
    auto vin = xsimd::batch<float, Arch>::load_aligned(&aInput[i]);
    auto vout = vin * vgain;
    vout.store_aligned(&aOutput[i]);
  }
};

template <class Arch>
void Engine<Arch>::AudioBufferCopyChannelWithScale(const float* aInput,
                                                  const float* aScale,
                                                  float* aOutput,
                                                  uint32_t aSize) {
  MOZ_ASSERT(is_aligned<Arch>(aInput), "aInput is aligned");
  MOZ_ASSERT(is_aligned<Arch>(aOutput), "aOutput is aligned");
  MOZ_ASSERT(is_aligned<Arch>(aScale), "aScale is aligned");
  MOZ_ASSERT((aSize % xsimd::batch<float, Arch>::size == 0),
             "requires tail processing");

  MOZ_UNROLL(4)
  for (unsigned i = 0; i < aSize; i += xsimd::batch<float, Arch>::size) {
    auto vscaled = xsimd::batch<float, Arch>::load_aligned(&aScale[i]);
    auto vin = xsimd::batch<float, Arch>::load_aligned(&aInput[i]);
    auto vout = vin * vscaled;
    vout.store_aligned(&aOutput[i]);
  }
};

template <class Arch>
void Engine<Arch>::AudioBufferInPlaceScale(float* aBlock, float aScale,
                                           uint32_t aSize) {
  MOZ_ASSERT(is_aligned<Arch>(aBlock), "aBlock is aligned");

  xsimd::batch<float, Arch> vgain(aScale);

  uint32_t aVSize = aSize & ~(xsimd::batch<float, Arch>::size - 1);
  MOZ_UNROLL(4)
  for (unsigned i = 0; i < aVSize; i += xsimd::batch<float, Arch>::size) {
    auto vin = xsimd::batch<float, Arch>::load_aligned(&aBlock[i]);
    auto vout = vin * vgain;
    vout.store_aligned(&aBlock[i]);
  }
  for (unsigned i = aVSize; i < aSize; ++i) aBlock[i] *= aScale;
};

template <class Arch>
void Engine<Arch>::AudioBufferInPlaceScale(float* aBlock, float* aScale,
                                           uint32_t aSize) {
  MOZ_ASSERT(is_aligned<Arch>(aBlock), "aBlock is aligned");
  MOZ_ASSERT(is_aligned<Arch>(aScale), "aScale is aligned");

  uint32_t aVSize = aSize & ~(xsimd::batch<float, Arch>::size - 1);
  MOZ_UNROLL(4)
  for (unsigned i = 0; i < aVSize; i += xsimd::batch<float, Arch>::size) {
    auto vin = xsimd::batch<float, Arch>::load_aligned(&aBlock[i]);
    auto vgain = xsimd::batch<float, Arch>::load_aligned(&aScale[i]);
    auto vout = vin * vgain;
    vout.store_aligned(&aBlock[i]);
  }
  for (uint32_t i = aVSize; i < aSize; ++i) {
    *aBlock++ *= *aScale++;
  }
};

template <class Arch>