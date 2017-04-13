// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef REGISTERS_H
#define REGISTERS_H

#include "magma_util/macros.h"
#include "register_bitfields.h"
#include "register_io.h"
#include "types.h"
#include <vector>

namespace registers {

// from intel-gfx-prm-osrc-skl-vol02c-commandreference-registers-part2.pdf p.733
class GmchGraphicsControl {
public:
    static constexpr uint32_t kOffset = 0x50;
    static constexpr uint32_t kGttSizeShift = 6;
    static constexpr uint32_t kGttSizeMask = 0x3;

    static uint32_t gtt_size(uint32_t val)
    {
        unsigned int size = (val >> kGttSizeShift) & kGttSizeMask;
        return (size == 0) ? 0 : (1 << size) * 1024 * 1024;
    }
};

// from intel-gfx-prm-osrc-bdw-vol02c-commandreference-registers_4.pdf p.712
class HardwareStatusPageAddress {
public:
    static constexpr uint32_t kOffset = 0x80;

    static void write(RegisterIo* reg_io, uint64_t mmio_base, uint32_t addr)
    {
        reg_io->Write32(mmio_base + kOffset, addr);
        reg_io->mmio()->PostingRead32(mmio_base + kOffset);
    }
};

// from intel-gfx-prm-osrc-skl-vol02c-commandreference-registers-part2.pdf p.500
class PatIndex {
public:
    static constexpr uint32_t kOffsetLow = 0x40E0;
    static constexpr uint32_t kOffsetHigh = 0x40E4;

    static constexpr uint8_t kUncacheable = 0;
    static constexpr uint8_t kWriteCombining = 1;
    static constexpr uint8_t kWriteThrough = 2;
    static constexpr uint8_t kWriteBack = 3;
    static constexpr uint8_t kMemTypeMask = 0x3;

    static constexpr uint8_t kEllc = 0;
    static constexpr uint8_t kLlc = 1;
    static constexpr uint8_t kLlcEllc = 2;
    static constexpr uint8_t kTargetCacheMask = 3;

    static constexpr uint8_t kLruAgeFromUncore = 0;
    static constexpr uint8_t kLruAgeZero = 1;
    static constexpr uint8_t kLruAgeNoChange = 2;
    static constexpr uint8_t kLruAgeThree = 3;
    static constexpr uint8_t kLruAgeMask = 0x3;

    static void write(RegisterIo* reg_io, uint64_t val)
    {
        reg_io->Write32(kOffsetLow, static_cast<uint32_t>(val));
        reg_io->Write32(kOffsetHigh, static_cast<uint32_t>(val >> 32));
    }

    static uint64_t ppat(unsigned int index, uint8_t lru_age, uint8_t target_cache,
                         uint8_t mem_type)
    {
        DASSERT((lru_age & ~kLruAgeMask) == 0);
        DASSERT((target_cache & ~kTargetCacheMask) == 0);
        DASSERT((mem_type & ~kMemTypeMask) == 0);
        uint64_t ppat = (lru_age << 4) | (target_cache << 2) | mem_type;
        return ppat << (index * 8);
    }
};

// from intel-gfx-prm-osrc-bdw-vol02c-commandreference-registers_4.pdf p.438
// and intel-gfx-prm-osrc-bdw-vol02d-commandreference-structures_3.pdf p.107
class ExeclistSubmitPort {
public:
    static constexpr uint32_t kSubmitOffset = 0x230;
    static constexpr uint32_t kStatusOffset = 0x234;

    static uint64_t context_descriptor(gpu_addr_t gpu_addr, uint32_t context_id, bool ppgtt_enable)
    {
        constexpr uint32_t kValid = 1;
        constexpr uint32_t kLegacyMode32bitPpgtt = 1 << 3;
        constexpr uint32_t kLegacyModePpgttEnable = 1 << 8;
        constexpr uint32_t kContextIdShift = 32;

        uint64_t desc = gpu_addr;
        desc |= kValid;
        desc |= kLegacyMode32bitPpgtt;
        if (ppgtt_enable)
            desc |= kLegacyModePpgttEnable;
        desc |= static_cast<uint64_t>(context_id) << kContextIdShift;
        return desc;
    }

    static void write(RegisterIo* reg_io, uint64_t mmio_base, uint64_t descriptor1,
                      uint64_t descriptor0)
    {
        uint32_t desc[4]{magma::upper_32_bits(descriptor1), magma::lower_32_bits(descriptor1),
                         magma::upper_32_bits(descriptor0), magma::lower_32_bits(descriptor0)};

        // The last write triggers the context load.
        reg_io->Write32(mmio_base + kSubmitOffset, desc[0]);
        reg_io->Write32(mmio_base + kSubmitOffset, desc[1]);
        reg_io->Write32(mmio_base + kSubmitOffset, desc[2]);
        reg_io->Write32(mmio_base + kSubmitOffset, desc[3]);

        reg_io->mmio()->PostingRead32(mmio_base + kStatusOffset);
    }
};

class ExeclistStatus {
public:
    static constexpr uint32_t kOffset = 0x234;
    static constexpr uint32_t kExeclistCurrentPointerShift = 0;
    static constexpr uint32_t kExeclistWritePointerShift = 1;
    static constexpr uint32_t kExeclistQueueFullShift = 2;

    static uint64_t read(RegisterIo* reg_io, uint64_t mmio_base)
    {
        // Hmm 64-bit read would be better but kOffset is not 64bit aligned
        uint64_t status = reg_io->Read32(mmio_base + kOffset + 4);
        status = (status << 32) | reg_io->Read32(mmio_base + kOffset);
        return status;
    }

    static uint32_t execlist_current_pointer(uint64_t status)
    {
        return (status >> kExeclistCurrentPointerShift) & 0x1;
    }

    static uint32_t execlist_write_pointer(uint64_t status)
    {
        return (status >> kExeclistWritePointerShift) & 0x1;
    }

    static bool execlist_queue_full(uint64_t status)
    {
        return (status >> kExeclistQueueFullShift) & 0x1;
    }
};

class ActiveHeadPointer {
public:
    static constexpr uint32_t kOffset = 0x74;
    static constexpr uint32_t kUpperOffset = 0x5C;

    static uint64_t read(RegisterIo* reg_io, uint64_t mmio_base)
    {
        uint64_t val = reg_io->Read32(mmio_base + kUpperOffset);
        val = (val << 32) | reg_io->Read32(mmio_base + kOffset);
        return val;
    }
};

// from intel-gfx-prm-osrc-bdw-vol02c-commandreference-registers_4.pdf p.75
class AllEngineFault {
public:
    static constexpr uint32_t kOffset = 0x4094;
    static constexpr uint32_t kValid = 1;
    static constexpr uint32_t kEngineShift = 12;
    static constexpr uint32_t kEngineMask = 0x3;
    static constexpr uint32_t kSrcShift = 3;
    static constexpr uint32_t kSrcMask = 0xFF;
    static constexpr uint32_t kTypeShift = 1;
    static constexpr uint32_t kTypeMask = 0x3;

    static uint32_t read(RegisterIo* reg_io) { return reg_io->Read32(kOffset); }
    static void clear(RegisterIo* reg_io) { reg_io->Write32(kOffset, 0); }

    static bool valid(uint32_t val) { return val & kValid; }
    static uint32_t engine(uint32_t val) { return (val >> kEngineShift) & kEngineMask; }
    static uint32_t src(uint32_t val) { return (val >> kSrcShift) & kSrcMask; }
    static uint32_t type(uint32_t val) { return (val >> kTypeShift) & kTypeMask; }
};

// from intel-gfx-prm-osrc-bdw-vol02c-commandreference-registers_4.pdf p.446
class FaultTlbReadData {
public:
    static constexpr uint32_t kOffset0 = 0x4B10;
    static constexpr uint32_t kOffset1 = 0x4B14;
    static constexpr uint32_t kGgttCycle = 1 << 4;

    static uint64_t addr(RegisterIo* reg_io)
    {
        return (static_cast<uint64_t>(reg_io->Read32(kOffset1) & 0xF) << 44) |
               (static_cast<uint64_t>(reg_io->Read32(kOffset0)) << 12);
    }

    static bool is_ggtt(RegisterIo* reg_io) { return reg_io->Read32(kOffset1) & kGgttCycle; }
};

// from intel-gfx-prm-osrc-bdw-vol02c-commandreference-registers_4.pdf p.493
class ForceWake {
public:
    enum Domain { GEN8, GEN9_RENDER };

    static constexpr uint32_t kOffset = 0xA188;
    static constexpr uint32_t kStatusOffset = 0x130044;

    static constexpr uint32_t kRenderOffset = 0xA278;
    static constexpr uint32_t kRenderStatusOffset = 0xD84;

    static void reset(RegisterIo* reg_io, Domain domain) { write(reg_io, domain, 0xFFFF, 0); }

    static void write(RegisterIo* reg_io, Domain domain, uint16_t mask, uint16_t val)
    {
        uint32_t val32 = mask;
        val32 = (val32 << 16) | val;
        switch (domain) {
            case GEN8:
                reg_io->Write32(kOffset, val32);
                break;
            case GEN9_RENDER:
                reg_io->Write32(kRenderOffset, val32);
                break;
        }
    }

    static uint16_t read_status(RegisterIo* reg_io, Domain domain)
    {
        switch (domain) {
            case GEN8:
                return static_cast<uint16_t>(reg_io->Read32(kStatusOffset));
            case GEN9_RENDER:
                return static_cast<uint16_t>(reg_io->Read32(kRenderStatusOffset));
        }
    }
};

// from intel-gfx-prm-osrc-bdw-vol02c-commandreference-registers_4.pdf p.618
class GraphicsMode {
public:
    static constexpr uint32_t kOffset = 0x29C;
    static constexpr uint32_t kExeclistEnable = 1 << 15;

    static void write(RegisterIo* reg_io, uint64_t mmio_base, uint16_t mask, uint16_t val)
    {
        uint32_t val32 = mask;
        val32 = (val32 << 16) | val;
        reg_io->Write32(mmio_base + kOffset, val32);
        reg_io->mmio()->PostingRead32(mmio_base + kOffset);
    }
};

class Pipe {
public:
    // Number of pipes that the hardware provides.
    static constexpr uint32_t kPipeCount = 3;
};

// from intel-gfx-prm-osrc-skl-vol02c-commandreference-registers-part2.pdf p.601
class DisplayPlaneSurfaceAddress : public RegisterBase {
public:
    static constexpr uint32_t kBaseAddr = 0x7019C;

    // This field omits the lower 12 bits of the address, so the address
    // must be 4k-aligned.
    static constexpr uint32_t kPageShift = 12;
    DEF_FIELD(31, 12, surface_base_address);

    DEF_BIT(3, ring_flip_source);

    // Get the instance of this register for Plane 1 of the given pipe.
    static auto Get(uint32_t pipe_number)
    {
        DASSERT(pipe_number < Pipe::kPipeCount);
        return RegisterAddr<DisplayPlaneSurfaceAddress>(kBaseAddr + 0x1000 * pipe_number);
    }
};

// from intel-gfx-prm-osrc-skl-vol02c-commandreference-registers-part2.pdf p.598
class DisplayPlaneSurfaceStride : public RegisterBase {
public:
    static constexpr uint32_t kBaseAddr = 0x70188;

    DEF_FIELD(9, 0, stride);

    // Get the instance of this register for Plane 1 of the given pipe.
    static auto Get(uint32_t pipe_number)
    {
        DASSERT(pipe_number < Pipe::kPipeCount);
        return RegisterAddr<DisplayPlaneSurfaceStride>(kBaseAddr + 0x1000 * pipe_number);
    }
};

class DisplayPlaneSurfaceSize : public RegisterBase {
public:
    static constexpr uint32_t kBaseAddr = 0x70190;

    DEF_FIELD(27, 16, height_minus_1);
    DEF_FIELD(12, 0, width_minus_1);

    // Get the instance of this register for Plane 1 of the given pipe.
    static auto Get(uint32_t pipe_number)
    {
        DASSERT(pipe_number < Pipe::kPipeCount);
        return RegisterAddr<DisplayPlaneSurfaceSize>(kBaseAddr + 0x1000 * pipe_number);
    }
};

// from intel-gfx-prm-osrc-skl-vol02c-commandreference-registers-part2.pdf p.559-566
class DisplayPlaneControl : public RegisterBase {
public:
    static constexpr uint32_t kBaseAddr = 0x70180;

    DEF_BIT(31, plane_enable);
    DEF_BIT(30, pipe_gamma_enable);
    DEF_BIT(29, remove_yuv_offset);
    DEF_BIT(28, yuv_range_correction_disable);
    DEF_FIELD(27, 24, source_pixel_format);
    DEF_BIT(23, pipe_csc_enable);
    DEF_FIELD(22, 21, key_enable);
    DEF_BIT(20, rgb_color_order);
    DEF_BIT(19, plane_yuv_to_rgb_csc_dis);
    DEF_BIT(18, plane_yuv_to_rgb_csc_format);
    DEF_FIELD(17, 16, yuv_422_byte_order);
    DEF_BIT(15, render_decompression);
    DEF_BIT(14, trickle_feed_enable);
    DEF_BIT(13, plane_gamma_disable);

    DEF_FIELD(12, 10, tiled_surface);
    enum Tiling { TILING_NONE = 0, TILING_X = 1, TILING_Y_LEGACY = 4, TILING_YF = 5 };

    DEF_BIT(9, async_address_update_enable);
    DEF_FIELD(7, 6, stereo_surface_vblank_mask);
    DEF_FIELD(5, 4, alpha_mode);
    DEF_BIT(3, allow_double_buffer_update_disable);
    DEF_FIELD(1, 0, plane_rotation);

    // Get the instance of this register for Plane 1 of the given pipe.
    static auto Get(uint32_t pipe_number)
    {
        DASSERT(pipe_number < Pipe::kPipeCount);
        return RegisterAddr<DisplayPlaneControl>(kBaseAddr + 0x1000 * pipe_number);
    }
};

// from intel-gfx-prm-osrc-skl-vol02c-commandreference-registers-part1.pdf p.444
class DisplayPipeInterrupt {
public:
    enum Pipe { PIPE_A };

    static constexpr uint32_t kMaskOffsetPipeA = 0x44404;
    static constexpr uint32_t kIdentityOffsetPipeA = 0x44408;
    static constexpr uint32_t kEnableOffsetPipeA = 0x4440C;
    static constexpr uint32_t kPlane1FlipDoneBit = 1 << 3;

    static void write_mask(RegisterIo* reg_io, Pipe pipe, uint32_t bits, bool enable)
    {
        uint32_t offset, val;
        switch (pipe) {
            case PIPE_A:
                offset = kMaskOffsetPipeA;
                break;
        }

        val = reg_io->Read32(offset);
        val = enable ? (val & ~bits) : val | bits;
        reg_io->Write32(offset, val);
    }

    static void write_enable(RegisterIo* reg_io, Pipe pipe, uint32_t bits, bool enable)
    {
        uint32_t offset, val;
        switch (pipe) {
            case PIPE_A:
                offset = kEnableOffsetPipeA;
                break;
        }

        val = reg_io->Read32(offset);
        val = enable ? (val | bits) : (val & ~bits);
        reg_io->Write32(offset, val);
    }

    static void process_identity_bits(RegisterIo* reg_io, Pipe pipe, uint32_t bits,
                                      bool* bits_present_out)
    {
        uint32_t offset, val;
        switch (pipe) {
            case PIPE_A:
                offset = kIdentityOffsetPipeA;
                break;
        }
        val = reg_io->Read32(offset);
        if ((*bits_present_out = val & bits))
            reg_io->Write32(offset, val | bits); // reset the event
    }
};

// from Intel-GFX-BSpec-NDA-SKL-20150707-b93797-r96240-Web register spec
class RenderPerformanceNormalFrequencyRequest {
public:
    static constexpr uint32_t kOffset = 0xA008;

    static void write_frequency_request_gen9(RegisterIo* reg_io, uint32_t mhz)
    {
        // Register in units of 16.66Mhz on skylake
        uint32_t val = mhz * 3 / 50;
        DASSERT(val <= 0x1ff);
        return reg_io->Write32(kOffset, val << 23);
    }
};

class RenderPerformanceStatus {
public:
    static constexpr uint32_t kOffset = 0xA01C;

    // Returns frequency in MHz
    static uint32_t read_current_frequency_gen9(RegisterIo* reg_io)
    {
        // Register in units of 16.66Mhz on skylake
        return (reg_io->Read32(kOffset) >> 23) * 50 / 3;
    }
};

class RenderPerformanceStateCapability {
public:
    static constexpr uint32_t kOffset = 0x140000 + 0x5998;

    // Returns frequency in Mhz
    static uint32_t read_rp0_frequency(RegisterIo* register_io)
    {
        // Register units are 50Mhz
        return (register_io->Read32(kOffset) & 0xff) * 50;
    }
};

// from intel-gfx-prm-osrc-skl-vol02c-commandreference-registers-part2.pdf p.741
class ResetControl {
public:
    static constexpr uint32_t kOffset = 0xd0;
    static constexpr uint32_t kRequestResetBit = 0;
    static constexpr uint32_t kReadyForResetBit = 1;

    static void request(RegisterIo* register_io, uint64_t mmio_base)
    {
        register_io->Write32(mmio_base + kOffset,
                             ((1 << kRequestResetBit) << 16) | (1 << kRequestResetBit));
    }

    static bool ready_for_reset(RegisterIo* register_io, uint64_t mmio_base)
    {
        return register_io->Read32(mmio_base + kOffset) & (1 << kReadyForResetBit);
    }
};

// from intel-gfx-prm-osrc-skl-vol02c-commandreference-registers-part1.pdf p.755
class GraphicsDeviceResetControl {
public:
    enum Engine { RENDER_ENGINE };

    static constexpr uint32_t kOffset = 0x941C;
    static constexpr uint32_t kRenderResetBit = 1;

    static void initiate_reset(RegisterIo* register_io, Engine engine)
    {
        switch (engine) {
            case RENDER_ENGINE:
                register_io->Write32(kOffset, (1 << kRenderResetBit));
                break;
        }
    }

    static bool is_reset_complete(RegisterIo* register_io, Engine engine)
    {
        switch (engine) {
            case RENDER_ENGINE:
                return (register_io->Read32(kOffset) & (1 << kRenderResetBit)) == 0;
        }
    }
};

// from intel-gfx-prm-osrc-skl-vol02c-commandreference-registers-part2.pdf p.10
class MasterInterruptControl {
public:
    static constexpr uint32_t kOffset = 0x44200;
    static constexpr uint32_t kRenderInterruptsPendingBitMask = 1 << 0;
    static constexpr uint32_t kDisplayEnginePipeAInterruptsPendingBit = 1 << 16;
    static constexpr uint32_t kEnableBitMask = 1 << 31;

    static void write(RegisterIo* register_io, bool enable)
    {
        register_io->Write32(kOffset, enable ? kEnableBitMask : 0);
    }
    static uint32_t read(RegisterIo* register_io) { return register_io->Read32(kOffset); }
};

class InterruptRegisterBase {
public:
    enum Engine { RENDER_ENGINE };
    enum Source { PAGE_FAULT, CONTEXT_SWITCH, USER };
    enum MaskOp { MASK, UNMASK };

    static constexpr uint32_t kUserInterruptBit = 1 << 0;
    static constexpr uint32_t kPageFaultBit = 1 << 7;
    static constexpr uint32_t kContextSwitchBit = 1 << 8;

    static void write(RegisterIo* register_io, uint64_t offset, Source source, bool set)
    {
        uint32_t bit;
        switch (source) {
            case USER:
                bit = kUserInterruptBit;
                break;
            case PAGE_FAULT:
                bit = kPageFaultBit;
                break;
            case CONTEXT_SWITCH:
                bit = kContextSwitchBit;
                break;
        }

        uint32_t val = register_io->Read32(offset);
        val = set ? (val | bit) : (val & ~bit);
        register_io->Write32(offset, val);
        register_io->mmio()->PostingRead32(offset);
        val = register_io->Read32(offset);
    }
};

class HardwareStatusMask : public InterruptRegisterBase {
public:
    static constexpr uint32_t kRenderOffset = 0x98;

    static void write(RegisterIo* register_io, uint64_t mmio_base, Engine engine, Source source,
                      MaskOp op)
    {
        switch (engine) {
            case RENDER_ENGINE:
                InterruptRegisterBase::write(register_io, mmio_base + kRenderOffset, source,
                                             op == MASK);
                break;
        }
    }
};

class GtInterruptMask0 : public InterruptRegisterBase {
public:
    static constexpr uint32_t kOffset = 0x44304;

    static void write(RegisterIo* register_io, Engine engine, Source source, MaskOp op)
    {
        switch (engine) {
            case RENDER_ENGINE:
                InterruptRegisterBase::write(register_io, kOffset, source, op == MASK);
                break;
        }
    }
};

class GtInterruptIdentity0 : public InterruptRegisterBase {
public:
    static constexpr uint32_t kOffset = 0x44308;

    static uint32_t read(RegisterIo* register_io, Engine engine)
    {
        switch (engine) {
            case RENDER_ENGINE:
                return register_io->Read32(kOffset);
        }
    }
    static void write(RegisterIo* register_io, Engine engine, Source source, MaskOp op)
    {
        switch (engine) {
            case RENDER_ENGINE:
                InterruptRegisterBase::write(register_io, kOffset, source, op == MASK);
                break;
        }
    }
};

class GtInterruptEnable0 : public InterruptRegisterBase {
public:
    static constexpr uint32_t kOffset = 0x4430C;

    static void write(RegisterIo* register_io, Engine engine, Source source, bool enable)
    {
        switch (engine) {
            case RENDER_ENGINE:
                InterruptRegisterBase::write(register_io, kOffset, source, enable);
                break;
        }
    }
};

// from intel-gfx-prm-osrc-skl-vol02c-commandreference-registers-part1.pdf p.764
class MemoryObjectControlState {
public:
    static constexpr uint32_t kGraphicsOffset = 0xC800;

    static constexpr uint32_t kCacheabilityShift = 0;
    static constexpr uint32_t kCacheShift = 2;
    static constexpr uint32_t kLruManagementShift = 4;

    enum Cacheability { PAGETABLE = 0, UNCACHED, WRITETHROUGH, WRITEBACK };
    enum Cache { LLC_ELLC = 2 };
    enum LruManagement { LRU_0 = 0, LRU_3 = 3 };

    static uint32_t format(Cacheability cacheability, Cache cache, LruManagement lru_management)
    {
        return (lru_management << kLruManagementShift) | (cache << kCacheShift) |
               (cacheability << kCacheabilityShift);
    }
};

// from intel-gfx-prm-osrc-skl-vol02c-commandreference-registers-part1.pdf p.1118
class LncfMemoryObjectControlState {
public:
    static constexpr uint32_t kOffset = 0xB020;

    static constexpr uint32_t kCacheabilityShift = 4;

    enum Cacheability { DIRECT = 0, UNCACHED, WRITETHROUGH, WRITEBACK };

    static uint16_t format(Cacheability cacheability) { return cacheability << kCacheabilityShift; }
};

// from intel-gfx-prm-osrc-skl-vol02c-commandreference-registers-part2.pdf p.403
class Fuse2ControlDwordMirror {
public:
    static constexpr uint32_t kOffset = 0x9120;

    static constexpr uint32_t kSliceEnableShift = 25;
    static constexpr uint32_t kSliceEnableMask = 0x7 << kSliceEnableShift;
    static constexpr uint32_t kSubsliceDisableShift = 20;
    static constexpr uint32_t kSubsliceDisableMask = 0xf << kSubsliceDisableShift;

    static void read(RegisterIo* register_io, uint32_t* slice_enable_mask_out,
                     uint32_t* subslice_enable_mask_out)
    {
        uint32_t val = register_io->Read32(kOffset);
        *slice_enable_mask_out = (val & kSliceEnableMask) >> kSliceEnableShift;
        *subslice_enable_mask_out = ((~val) & kSubsliceDisableMask) >> kSubsliceDisableShift;
    }
};

// from intel-gfx-prm-osrc-skl-vol02c-commandreference-registers-part2.pdf p.398
class MirrorEuDisable {
public:
    static constexpr uint32_t kOffset = 0x9134;

    static constexpr uint32_t kMaxSliceCount = 3;
    static constexpr uint32_t kMaxSubsliceCount = 4;
    static constexpr uint32_t kEuPerSubslice = 8;
    static constexpr uint32_t kSubsliceMask = 0xff;

    static_assert(kMaxSubsliceCount * kEuPerSubslice == sizeof(uint32_t) * 8,
                  "eu/subslice math is wrong");
    static_assert(kSubsliceMask == (1 << kEuPerSubslice) - 1, "wrong kSubsliceMask");

    static void read(RegisterIo* register_io, uint8_t slice,
                     std::vector<uint32_t>& eu_disable_mask_out)
    {
        DASSERT(slice < kMaxSliceCount);
        uint32_t val = register_io->Read32(kOffset + slice * sizeof(uint32_t));

        eu_disable_mask_out.clear();

        for (uint32_t subslice = 0; subslice < kMaxSubsliceCount; subslice++) {
            eu_disable_mask_out.push_back(val & kSubsliceMask);
            val >>= kEuPerSubslice;
        }
    }
};

// PWR_WELL_CTL: Power well control.  This allows enabling or disabling
// power to various "power wells" (groups of functional units).
// from intel-gfx-prm-osrc-skl-vol02c-commandreference-registers-part2.pdf
class PowerWellControl2 : public RegisterBase {
public:
    DEF_BIT(31, power_well_2_request);
    DEF_BIT(30, power_well_2_state);
    DEF_BIT(29, power_well_1_request);
    DEF_BIT(28, power_well_1_state);
    DEF_BIT(9, ddi_d_io_power_request);
    DEF_BIT(8, ddi_d_io_power_state);
    DEF_BIT(7, ddi_c_io_power_request);
    DEF_BIT(6, ddi_c_io_power_state);
    DEF_BIT(5, ddi_b_io_power_request);
    DEF_BIT(4, ddi_b_io_power_state);
    DEF_BIT(3, ddi_a_and_e_io_power_request);
    DEF_BIT(2, ddi_a_and_e_io_power_state);
    DEF_BIT(1, misc_io_power_request);
    DEF_BIT(0, misc_io_power_state);

    static auto Get() { return RegisterAddr<PowerWellControl2>(0x45404); }
};

} // namespace

#endif // REGISTERS_H
