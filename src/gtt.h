// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GTT_H
#define GTT_H

#include "address_space.h"
#include "magma_util/address_space_allocator.h"
#include "platform_buffer.h"
#include "platform_pci_device.h"
#include "register_io.h"
#include <memory>

class Gtt : public AddressSpace {
public:
    Gtt(std::shared_ptr<GpuMappingCache> cache);

    uint64_t Size() const override { return size_; }

    bool Init(uint64_t gtt_size, magma::PlatformPciDevice* platform_device);

    // AddressSpace overrides
    bool Alloc(size_t size, uint8_t align_pow2, uint64_t* addr_out) override;
    bool Free(uint64_t addr) override;

    bool Clear(uint64_t addr) override;
    bool Insert(uint64_t addr, magma::PlatformBuffer* buffer, uint64_t offset, uint64_t length,
                CachingType caching_type) override;

private:
    uint64_t pte_mmio_offset() { return mmio_->size() / 2; }

    magma::PlatformBuffer* scratch_buffer() { return scratch_.get(); }

    bool MapGttMmio(magma::PlatformPciDevice* platform_device);
    bool InitScratch();
    bool InitPageTables(uint64_t start);
    bool Clear(uint64_t start, uint64_t length);

private:
    std::unique_ptr<magma::PlatformMmio> mmio_;
    std::unique_ptr<magma::PlatformBuffer> scratch_;
    std::unique_ptr<magma::AddressSpaceAllocator> allocator_;
    uint64_t scratch_bus_addr_;
    uint64_t size_;

    friend class TestGtt;
};

#endif // GTT_H
