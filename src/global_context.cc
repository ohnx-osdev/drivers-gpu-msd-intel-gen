// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "global_context.h"

bool GlobalContext::Map(AddressSpace* address_space, EngineCommandStreamerId id)
{
    DLOG("Map for engine %d", id);

    if (!MapGpu(address_space, id))
        return DRETF(false, "failed to map");

    // Address space check is ok; if we're already mapped then we're done.
    if (status_page_map_.find(id) != status_page_map_.end())
        return true;

    gpu_addr_t gpu_addr;
    if (!GetGpuAddress(id, &gpu_addr))
        return DRETF(false, "failed to get gpu address");

    void* cpu_addr;
    if (!get_context_buffer(id)->platform_buffer()->MapCpu(&cpu_addr))
        return DRETF(false, "failed to get cpu address");

    status_page_map_[id] = PerEngineHardwareStatus{
        gpu_addr, cpu_addr, std::unique_ptr<HardwareStatusPage>(new HardwareStatusPage(this, id))};

    return true;
}

bool GlobalContext::Unmap(AddressSpace* address_space, EngineCommandStreamerId id)
{
    DLOG("Unmap for engine %d", id);

    auto iter = status_page_map_.find(id);
    if (iter == status_page_map_.end())
        return DRETF(false, "not mapped");

    if (!UnmapGpu(address_space, id))
        return DRETF(false, "failed to map");

    if (!get_context_buffer(id)->platform_buffer()->UnmapCpu())
        return DRETF(false, "failed to get cpu address");

    status_page_map_.erase(iter);

    return true;
}
