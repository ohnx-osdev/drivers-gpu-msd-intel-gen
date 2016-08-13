// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "msd_intel_device.h"
#include "magma_util/dlog.h"
#include "magma_util/macros.h"

MsdIntelDevice::MsdIntelDevice() { magic_ = kMagic; }

std::unique_ptr<MsdIntelConnection> MsdIntelDevice::Open(msd_client_id client_id)
{
    return std::unique_ptr<MsdIntelConnection>(new MsdIntelConnection());
}

bool MsdIntelDevice::Init(void* device_handle)
{
    DASSERT(!platform_device_);

    DLOG("Init device_handle %p", device_handle);

    platform_device_ = magma::PlatformDevice::Create(device_handle);
    if (!platform_device_)
        return DRETF(false, "failed to create platform device");

    std::unique_ptr<magma::PlatformMmio> mmio(
        platform_device_->CpuMapPciMmio(0, magma::PlatformMmio::CACHE_POLICY_UNCACHED_DEVICE));
    if (!mmio)
        return DRETF(false, "failed to map pci bar 0");

    register_io_ = std::shared_ptr<RegisterIo>(new RegisterIo(std::move(mmio)));

    return true;
}

//////////////////////////////////////////////////////////////////////////////

msd_connection* msd_device_open(msd_device* dev, msd_client_id client_id)
{
    // here we open the connection and transfer ownership of the result across the ABI
    return MsdIntelDevice::cast(dev)->Open(client_id).release();
}

uint32_t msd_device_get_id(msd_device* dev) { return MsdIntelDevice::cast(dev)->device_id(); }
