/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_AUDIOSCRATCHALLOCATOR_H_
#define MOZILLA_AUDIOSCRATCHALLOCATOR_H_

#include "mozilla/Assertions.h"
#include "mozilla/Span.h"
#include "nsTArray.h"
#ifdef DEBUG
#  include "prthread.h"
#endif

namespace mozilla {

/**
 * Bump allocator for per-render-quantum scratch memory shared across all
 * AudioNodeTrack engines on a single MediaTrackGraph.
 *
 * One instance lives on MediaTrackGraph, sized at graph creation time.
 * AudioNodeTrack creates a Scope around each ProcessBlock() call, which
 * resets the slab on entry and restores it on exit. Engine code allocates
 * via AudioNodeTrack::GetScratch<T>(count).
 *
 * All allocations are 16-byte aligned (suitable for SIMD).
 */
class AudioScratchAllocator {
 public:
  // aMaxAllocations is the maximum number of Alloc() calls per Scope.
  // Each call may waste up to kAlign-1 bytes on alignment; the slab is
  // sized to guarantee all allocations succeed.
  void Init(size_t aDataBytes, size_t aMaxAllocations = 16) {
    constexpr size_t kAlign = 16;
    mData.SetLength(aDataBytes + (kAlign - 1) * aMaxAllocations);
  }

  template <typename T>
  Span<T> Alloc(uint32_t aCount) {
#ifdef DEBUG
    MOZ_ASSERT(mScopeActive, "Alloc called outside of a Scope");
    AssertCorrectThread();
#endif
    constexpr uintptr_t kAlign = 16;
    uint8_t* base = mData.Elements() + mOffset;
    uint8_t* aligned = reinterpret_cast<uint8_t*>(
        (reinterpret_cast<uintptr_t>(base) + kAlign - 1) & ~(kAlign - 1));
    size_t bytes = aCount * sizeof(T);
    MOZ_ASSERT(aligned + bytes <= mData.Elements() + mData.Length(),
               "AudioScratchAllocator: out of scratch space");
    T* ptr = reinterpret_cast<T*>(aligned);
    mOffset = (aligned - mData.Elements()) + bytes;
    return {ptr, aCount};
  }

  // RAII guard that resets the allocator at the start of a ProcessBlock call
  // and restores it when the call returns. Scopes must not nest.
  class Scope {
   public:
    // Saves the current offset and resets to 0 so the full slab is available.
    // mScopeActive enforces that scopes don't nest; C++ LIFO destruction
    // ensures the offset is restored in the right order if they somehow did.
    explicit Scope(AudioScratchAllocator& aAlloc)
        : mAlloc(aAlloc), mSavedOffset(aAlloc.mOffset) {
#ifdef DEBUG
      MOZ_ASSERT(!aAlloc.mScopeActive,
                 "AudioScratchAllocator scopes must not nest");
      aAlloc.AssertCorrectThread();
      aAlloc.mScopeActive = true;
#endif
      aAlloc.mOffset = 0;
    }
    ~Scope() {
#ifdef DEBUG
      mAlloc.mScopeActive = false;
#endif
      mAlloc.mOffset = mSavedOffset;
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    AudioScratchAllocator& mAlloc;
    const size_t mSavedOffset;
  };

 private:
#ifdef DEBUG
  void AssertCorrectThread() {
    PRThread* current = PR_GetCurrentThread();
    if (!mThread) {
      mThread = current;
      return;
    }
    MOZ_ASSERT(current == mThread,
               "AudioScratchAllocator used from wrong thread");
  }
  PRThread* mThread = nullptr;
#endif

  nsTArray<uint8_t> mData;
  size_t mOffset = 0;
#ifdef DEBUG
  bool mScopeActive = false;
#endif
};

}  // namespace mozilla

#endif  // MOZILLA_AUDIOSCRATCHALLOCATOR_H_
