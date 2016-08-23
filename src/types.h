// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TYPES_H
#define TYPES_H

typedef uint64_t gen_pte_t;

using gpu_addr_t = uint64_t;

enum CachingType {
    CACHING_NONE,
    CACHING_LLC,
    CACHING_WRITE_THROUGH,
};

enum AddressSpaceId {
    ADDRESS_SPACE_GTT,
    ADDRESS_SPACE_PPGTT,
};

enum EngineCommandStreamerId {
    RENDER_COMMAND_STREAMER,
};

enum MemoryDomain {
    MEMORY_DOMAIN_CPU,
};

#endif // TYPES_H
