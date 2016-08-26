// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MSD_INTEL_CONNECTION_H
#define MSD_INTEL_CONNECTION_H

#include "engine_command_streamer.h"
#include "magma_util/macros.h"
#include "msd.h"
#include "msd_intel_context.h"
#include <memory>

class MsdIntelConnection : public msd_connection, public ClientContext::Owner {
public:
    class Owner {
    public:
        virtual HardwareStatusPage* hardware_status_page(EngineCommandStreamerId id) = 0;
    };

    MsdIntelConnection(Owner* owner) : owner_(owner) { magic_ = kMagic; }

    virtual ~MsdIntelConnection() {}

    std::unique_ptr<MsdIntelContext> CreateContext()
    {
        // Backing store creation deferred until context is used.
        return std::unique_ptr<MsdIntelContext>(new ClientContext(this));
    }

    static MsdIntelConnection* cast(msd_connection* connection)
    {
        DASSERT(connection);
        DASSERT(connection->magic_ == kMagic);
        return static_cast<MsdIntelConnection*>(connection);
    }

private:
    // ClientContext::Owner
    HardwareStatusPage* hardware_status_page(EngineCommandStreamerId id) override
    {
        return owner_->hardware_status_page(id);
    }

    static const uint32_t kMagic = 0x636f6e6e; // "conn" (Connection)

    Owner* owner_;
};

#endif // MSD_INTEL_CONNECTION_H
