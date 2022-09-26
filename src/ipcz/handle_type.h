// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IPCZ_SRC_IPCZ_HANDLE_TYPE_H_
#define IPCZ_SRC_IPCZ_HANDLE_TYPE_H_

#include <cstdint>

#include "ipcz/ipcz.h"

namespace ipcz {

// Identifies the type of each IpczHandle attached to a parcel.
enum class HandleType : uint32_t {
  // A portal handle consumes the next available RouterDescriptor in the
  // parcel. It does not consume any other data, or any OS handles.
  kPortal = 0,

  // A box handle consumes the next available element in the parcel's
  // DriverObject array and wraps it as a Box object.
  kBox = 1,

  // A placeholder for a box handle in a split parcel transmission. This occurs
  // in the directly transmitted half of the parcel, and it signifies the
  // existence of a corresponding DriverObject in the relayed half.
  kRelayedBox = 2,
};

}  // namespace ipcz

#endif  // IPCZ_SRC_IPCZ_HANDLE_TYPE_H_
