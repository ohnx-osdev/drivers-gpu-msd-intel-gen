// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "msd_intel_buffer.h"
#include "address_space.h"
#include "gpu_mapping.h"
#include "msd.h"

MsdIntelBuffer::MsdIntelBuffer(std::unique_ptr<magma::PlatformBuffer> platform_buf)
    : platform_buf_(std::move(platform_buf))
{
}

std::unique_ptr<MsdIntelBuffer> MsdIntelBuffer::Import(uint32_t handle)
{
    auto platform_buf = magma::PlatformBuffer::Import(handle);
    if (!platform_buf)
        return DRETP(nullptr,
                     "MsdIntelBuffer::Create: Could not create platform buffer from token");

    return std::unique_ptr<MsdIntelBuffer>(new MsdIntelBuffer(std::move(platform_buf)));
}

std::unique_ptr<MsdIntelBuffer> MsdIntelBuffer::Create(uint64_t size, const char* name)
{
    auto platform_buf = magma::PlatformBuffer::Create(size, name);
    if (!platform_buf)
        return DRETP(nullptr, "MsdIntelBuffer::Create: Could not create platform buffer from size");

    return std::unique_ptr<MsdIntelBuffer>(new MsdIntelBuffer(std::move(platform_buf)));
}

std::shared_ptr<GpuMapping> MsdIntelBuffer::ShareBufferMapping(std::unique_ptr<GpuMapping> mapping)
{
    if (mapping->buffer() != this)
        return DRETP(nullptr, "incorrect buffer");

    std::shared_ptr<GpuMapping> shared_mapping = std::move(mapping);

    shared_mappings_[shared_mapping.get()] = shared_mapping;

    return shared_mapping;
}

std::shared_ptr<GpuMapping>
MsdIntelBuffer::FindBufferMapping(std::shared_ptr<AddressSpace> address_space, uint64_t offset,
                                  uint64_t length, uint32_t alignment)
{
    for (auto map_node : shared_mappings_) {
        std::shared_ptr<GpuMapping> shared_mapping = map_node.second.lock();
        if (!shared_mapping || shared_mapping->address_space().expired())
            continue;

        gpu_addr_t gpu_addr = shared_mapping->gpu_addr();
        if (shared_mapping->address_space().lock() == address_space &&
            shared_mapping->offset() == offset &&
            shared_mapping->length() == address_space->GetMappedSize(length) &&
            (alignment == 0 || magma::round_up(gpu_addr, alignment) == gpu_addr))
            return shared_mapping;
    }

    return nullptr;
}

std::vector<std::shared_ptr<GpuMapping>>
MsdIntelBuffer::GetSharedMappings(AddressSpace* address_space)
{
    std::vector<std::shared_ptr<GpuMapping>> mappings;

    for (auto iter = shared_mappings_.begin(); iter != shared_mappings_.end();) {
        std::shared_ptr<GpuMapping> mapping = iter->second.lock();
        std::shared_ptr<AddressSpace> mapping_address_space =
            mapping ? mapping->address_space().lock() : nullptr;

        if (!mapping_address_space) {
            iter = shared_mappings_.erase(iter);
            continue;
        }

        if (mapping_address_space.get() == address_space)
            mappings.emplace_back(std::move(mapping));

        iter++;
    }

    return mappings;
}

void MsdIntelBuffer::RemoveSharedMapping(GpuMapping* mapping) { shared_mappings_.erase(mapping); }

void MsdIntelBuffer::DecrementInflightCounter()
{
    DASSERT(inflight_counter_ > 0);

    while (true) {
        uint64_t counter = inflight_counter_;
        if (counter & 0xFFFFFFFF00000000) {
            uint64_t new_counter = counter - 0x0000000100000001;
            if (inflight_counter_.compare_exchange_strong(counter, new_counter)) {
                if (new_counter == 0)
                    wait_rendering_event_->Signal();
                break;
            }
        } else if (inflight_counter_.compare_exchange_strong(counter, counter - 1))
            break;
    }
}

void MsdIntelBuffer::WaitRendering()
{
    if (!wait_rendering_event_)
        wait_rendering_event_ = magma::PlatformEvent::Create();

    std::unique_lock<std::mutex> lock(wait_rendering_mutex_);

    while (true) {
        uint64_t counter = inflight_counter_;
        if (counter == 0)
            return;
        if (inflight_counter_.compare_exchange_strong(counter, counter | (counter << 32)))
            break;
    };

    while (true) {
        constexpr uint32_t kTimeoutMs = 5000;
        bool result = wait_rendering_event_->Wait(kTimeoutMs);
        if (result)
            break;
        magma::log(magma::LOG_WARNING, "WaitRendering timedout after %u ms", kTimeoutMs);
    }

    wait_rendering_event_ = magma::PlatformEvent::Create();
}

//////////////////////////////////////////////////////////////////////////////

msd_buffer_t* msd_buffer_import(uint32_t handle)
{
    auto buffer = MsdIntelBuffer::Import(handle);
    if (!buffer)
        return DRETP(nullptr, "MsdIntelBuffer::Create failed");
    return new MsdIntelAbiBuffer(std::move(buffer));
}

void msd_buffer_destroy(msd_buffer_t* buf) { delete MsdIntelAbiBuffer::cast(buf); }
