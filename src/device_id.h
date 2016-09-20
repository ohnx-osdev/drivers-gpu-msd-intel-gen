// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_ID_H
#define DEVICE_ID_H

#include <stdint.h>

class DeviceId {
public:
    static bool is_gen8(uint32_t device_id) { return device_id == 0x1616; }
    static bool is_gen9(uint32_t device_id) { return device_id == 0x1916; }
};

#endif // DEVICE_ID_H
