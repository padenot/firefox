/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBNN_WEBNNUTILS_H_
#define DOM_WEBNN_WEBNNUTILS_H_

#include "mozilla/CheckedInt.h"
#include "mozilla/Maybe.h"
#include "mozilla/Span.h"
#include "mozilla/dom/BindingDeclarations.h"
#include "mozilla/dom/WebNNBinding.h"
#include "nsTArray.h"

namespace mozilla::webnn {

inline size_t ElementSize(dom::MLOperandDataType aType) {
  switch (aType) {
    case dom::MLOperandDataType::Float32:
    case dom::MLOperandDataType::Int32:
    case dom::MLOperandDataType::Uint32:
      return 4;
    case dom::MLOperandDataType::Float16:
      return 2;
    case dom::MLOperandDataType::Int64:
    case dom::MLOperandDataType::Uint64:
      return 8;
    case dom::MLOperandDataType::Int8:
    case dom::MLOperandDataType::Uint8:
      return 1;
  }
  MOZ_CRASH("Unexpected MLOperandDataType");
}

template <typename T>
nsTArray<T> ToArray(const dom::Sequence<T>& aSequence) {
  nsTArray<T> array;
  array.AppendElements(aSequence.Elements(), aSequence.Length());
  return array;
}

template <typename T>
Maybe<nsTArray<T>> ToOptionalArray(
    const dom::Optional<dom::Sequence<T>>& aSequence) {
  return aSequence.WasPassed() ? Some(ToArray(aSequence.Value())) : Nothing();
}

template <typename T>
Maybe<T> ToMaybe(const dom::Optional<T>& aValue) {
  return aValue.WasPassed() ? Some(aValue.Value()) : Nothing();
}

inline nsTArray<uint32_t> ToShape(const dom::Sequence<uint32_t>& aShape) {
  return ToArray(aShape);
}

// Returns Nothing() on overflow.
inline Maybe<size_t> ByteLength(dom::MLOperandDataType aType,
                                Span<const uint32_t> aShape) {
  CheckedInt<size_t> length = ElementSize(aType);
  for (uint32_t dim : aShape) {
    length *= dim;
  }
  return length.isValid() ? Some(length.value()) : Nothing();
}

}  // namespace mozilla::webnn

#endif  // DOM_WEBNN_WEBNNUTILS_H_
