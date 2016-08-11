// Copyright 2016 The Fuchsia Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MSD_DEVICE_H
#define MSD_DEVICE_H

#include "magma_util/macros.h"
#include "msd.h"
#include "msd_connection.h"
#include <ddk/device.h>

class MsdDevice : public msd_device {
public:
    // This takes ownership of the connection so that ownership can be
    // transferred across the MSD ABI by the caller
    std::unique_ptr<MsdConnection> Open(msd_client_id client_id);

    uint32_t device_id() { return device_id_; }

    static MsdDevice* cast(msd_device* dev)
    {
        DASSERT(dev);
        DASSERT(dev->magic_ == kMagic);
        return static_cast<MsdDevice*>(dev);
    }

private:
    MsdDevice();

    static const uint32_t kMagic = 0x64657669; //"devi"

    uint32_t device_id_{};

    friend class MsdDriver;
};

#endif // MSD_DEVICE_H
