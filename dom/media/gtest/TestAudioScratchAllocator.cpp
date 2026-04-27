/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AudioScratchAllocator.h"
#include "gtest/gtest.h"

using namespace mozilla;

TEST(AudioScratchAllocator, Alignment)
{
  AudioScratchAllocator alloc;
  alloc.Init(4096);
  AudioScratchAllocator::Scope scope(alloc);

  auto f1 = alloc.Alloc<float>(128);
  auto b1 = alloc.Alloc<bool>(128);
  auto f2 = alloc.Alloc<float>(128);

  EXPECT_EQ(reinterpret_cast<uintptr_t>(f1.data()) % 16, 0u);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(b1.data()) % 16, 0u);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(f2.data()) % 16, 0u);
}

TEST(AudioScratchAllocator, ScopeResets)
{
  AudioScratchAllocator alloc;
  alloc.Init(4096);

  float* first = nullptr;
  {
    AudioScratchAllocator::Scope scope(alloc);
    first = alloc.Alloc<float>(128).data();
  }
  {
    AudioScratchAllocator::Scope scope(alloc);
    EXPECT_EQ(alloc.Alloc<float>(128).data(), first);
  }
}

TEST(AudioScratchAllocator, NoOverlap)
{
  AudioScratchAllocator alloc;
  alloc.Init(4096);
  AudioScratchAllocator::Scope scope(alloc);

  auto a = alloc.Alloc<float>(128);
  auto b = alloc.Alloc<float>(128);

  EXPECT_GE(reinterpret_cast<uintptr_t>(b.data()),
            reinterpret_cast<uintptr_t>(a.data()) + 128 * sizeof(float));
}

// Degenerate blockSize=1: each float alloc is 4 bytes but rounded up to 16.
// 9 allocations need 9*(4+15)=171 bytes; Init(9*4) = 36 bytes would overflow
// without the alignment padding added by Init().
TEST(AudioScratchAllocator, PaddingFor9Arrays)
{
  const uint32_t blockSize = 1;
  AudioScratchAllocator alloc;
  alloc.Init(9 * blockSize * sizeof(float));
  AudioScratchAllocator::Scope scope(alloc);
  for (int i = 0; i < 9; ++i) {
    auto buf = alloc.Alloc<float>(blockSize);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(buf.data()) % 16, 0u);
  }
}
