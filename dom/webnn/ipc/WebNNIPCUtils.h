/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBNN_IPC_WEBNNIPCUTILS_H_
#define DOM_WEBNN_IPC_WEBNNIPCUTILS_H_

#include "mozilla/dom/BindingIPCUtils.h"
#include "mozilla/dom/WebNNBinding.h"

namespace IPC {

template <>
struct ParamTraits<mozilla::dom::MLOperandDataType>
    : public mozilla::dom::WebIDLEnumSerializer<
          mozilla::dom::MLOperandDataType> {};

template <>
struct ParamTraits<mozilla::dom::MLPowerPreference>
    : public mozilla::dom::WebIDLEnumSerializer<
          mozilla::dom::MLPowerPreference> {};

}  // namespace IPC

#endif  // DOM_WEBNN_IPC_WEBNNIPCUTILS_H_
