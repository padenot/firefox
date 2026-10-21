/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBNN_VALIDATION_VALIDATIONUTILS_H_
#define DOM_WEBNN_VALIDATION_VALIDATIONUTILS_H_

#include <algorithm>

#include "mozilla/Maybe.h"
#include "mozilla/Span.h"
#include "nsTArray.h"

namespace mozilla::webnn {

// Bidirectional broadcasting, as in
// https://webmachinelearning.github.io/webnn/#biblio-numpy-broadcasting-rule
inline Maybe<nsTArray<uint32_t>> BroadcastShapes(Span<const uint32_t> aA,
                                                 Span<const uint32_t> aB) {
  size_t rank = std::max(aA.Length(), aB.Length());
  nsTArray<uint32_t> result;
  result.SetLength(rank);
  for (size_t i = 0; i < rank; i++) {
    uint32_t a = i < aA.Length() ? aA[aA.Length() - 1 - i] : 1;
    uint32_t b = i < aB.Length() ? aB[aB.Length() - 1 - i] : 1;
    if (a != b && a != 1 && b != 1) {
      return Nothing();
    }
    result[rank - 1 - i] = a == 1 ? b : a;
  }
  return Some(std::move(result));
}

}  // namespace mozilla::webnn

#endif  // DOM_WEBNN_VALIDATION_VALIDATIONUTILS_H_
