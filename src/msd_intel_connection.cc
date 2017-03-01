// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "msd_intel_connection.h"
#include "magma_util/dlog.h"
#include "ppgtt.h"

void msd_connection_close(msd_connection* connection)
{
    delete MsdIntelAbiConnection::cast(connection);
}

msd_context* msd_connection_create_context(msd_connection* abi_connection)
{
    auto connection = MsdIntelAbiConnection::cast(abi_connection)->ptr();

    // Backing store creation deferred until context is used.
    return new MsdIntelAbiContext(
        std::make_unique<ClientContext>(connection, connection->per_process_gtt()));
}

magma_status_t msd_connection_wait_rendering(msd_connection* abi_connection, msd_buffer* buffer)
{
    auto connection = MsdIntelAbiConnection::cast(abi_connection)->ptr();

    if (connection->context_killed())
        return DRET(MAGMA_STATUS_CONTEXT_KILLED);

    MsdIntelAbiBuffer::cast(buffer)->ptr()->WaitRendering();

    if (connection->context_killed())
        return DRET(MAGMA_STATUS_CONTEXT_KILLED);

    return MAGMA_STATUS_OK;
}

std::unique_ptr<MsdIntelConnection>
MsdIntelConnection::Create(Owner* owner, std::shared_ptr<magma::PlatformBuffer> scratch_buffer)
{
    return std::unique_ptr<MsdIntelConnection>(new MsdIntelConnection(
        owner, PerProcessGtt::Create(std::move(scratch_buffer), owner->mapping_cache())));
}
