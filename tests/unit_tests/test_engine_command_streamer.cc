// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "engine_command_streamer.h"
#include "mock/mock_address_space.h"
#include "mock/mock_mmio.h"
#include "registers.h"
#include "render_init_batch.h"
#include "sequencer.h"
#include "gtest/gtest.h"

class TestContext {
public:
    static MsdIntelBuffer* get_context_buffer(MsdIntelContext* context, EngineCommandStreamerId id)
    {
        return context->get_context_buffer(id);
    }
};

class MockStatusPageBuffer {
public:
    MockStatusPageBuffer()
    {
        cpu_addr = malloc(PAGE_SIZE);
        gpu_addr = 0x10000;
    }

    ~MockStatusPageBuffer() { free(cpu_addr); }

    void* cpu_addr;
    gpu_addr_t gpu_addr;
};

class TestEngineCommandStreamer : public EngineCommandStreamer::Owner,
                                  public ClientContext::Owner,
                                  public HardwareStatusPage::Owner {
public:
    static constexpr uint32_t kFirstSequenceNumber = 5;

    TestEngineCommandStreamer()
    {
        register_io_ =
            std::unique_ptr<RegisterIo>(new RegisterIo(MockMmio::Create(8 * 1024 * 1024)));

        context_ = std::unique_ptr<MsdIntelContext>(new ClientContext(this));

        mock_status_page_ = std::unique_ptr<MockStatusPageBuffer>(new MockStatusPageBuffer());

        address_space_ = std::unique_ptr<AddressSpace>(new MockAddressSpace(0, PAGE_SIZE * 100));

        engine_cs_ = RenderEngineCommandStreamer::Create(this, address_space_.get());

        sequencer_ = std::unique_ptr<Sequencer>(new Sequencer(kFirstSequenceNumber));

        hw_status_page_ =
            std::unique_ptr<HardwareStatusPage>(new HardwareStatusPage(this, engine_cs_->id()));
    }

    void InitContext()
    {
        auto buffer = TestContext::get_context_buffer(context_.get(), engine_cs_->id());
        EXPECT_EQ(buffer, nullptr);

        EXPECT_TRUE(engine_cs_->InitContext(context_.get()));

        buffer = TestContext::get_context_buffer(context_.get(), engine_cs_->id());
        EXPECT_NE(buffer, nullptr);
        EXPECT_EQ(buffer->platform_buffer()->size(), PAGE_SIZE * 20ul);

        void* addr;
        EXPECT_TRUE(buffer->platform_buffer()->MapCpu(&addr));

        uint32_t* state = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(addr) + PAGE_SIZE);
        EXPECT_EQ(state[1], 0x1100101Bul);
        EXPECT_EQ(state[2], 0x2244ul);
        EXPECT_EQ(state[3], 0x00090009ul);
        EXPECT_EQ(state[4], 0x2034ul);
        EXPECT_EQ(state[5], 0ul);
        EXPECT_EQ(state[6], 0x2030ul);
        EXPECT_EQ(state[7], 0u);
        EXPECT_EQ(state[8], 0x2038ul);
        // state[9] is not set until later
        EXPECT_EQ(state[0xA], 0x203Cul);
        EXPECT_EQ(state[0xB], (31ul * PAGE_SIZE) | 1);
        EXPECT_EQ(state[0xC], 0x2168ul);
        EXPECT_EQ(state[0xD], 0ul);
        EXPECT_EQ(state[0xE], 0x2140ul);
        EXPECT_EQ(state[0xF], 0ul);
        EXPECT_EQ(state[0x10], 0x2110ul);
        EXPECT_EQ(state[0x11], 1ul << 5);
        EXPECT_EQ(state[0x12], 0x211Cul);
        EXPECT_EQ(state[0x13], 0ul);
        EXPECT_EQ(state[0x14], 0x2114ul);
        EXPECT_EQ(state[0x15], 0ul);
        EXPECT_EQ(state[0x16], 0x2118ul);
        EXPECT_EQ(state[0x17], 0ul);
        EXPECT_EQ(state[0x18], 0x21C0ul);
        EXPECT_EQ(state[0x19], 0ul);
        EXPECT_EQ(state[0x1A], 0x21C4ul);
        EXPECT_EQ(state[0x1B], 0ul);
        EXPECT_EQ(state[0x1C], 0x21C8ul);
        EXPECT_EQ(state[0x1D], 0ul);
        EXPECT_EQ(state[0x1E], 0x23A8ul);
        EXPECT_EQ(state[0x1F], 0ul);
        EXPECT_EQ(state[0x21], 0x11001011ul);
        EXPECT_EQ(state[0x24], 0x228Cul);
        // TODO(MA-64) - check ppgtt pdp addresses
        // EXPECT_EQ(state[0x25], pdp3_upper);
        EXPECT_EQ(state[0x26], 0x2288ul);
        // EXPECT_EQ(state[0x27], pdp3_lower);
        EXPECT_EQ(state[0x28], 0x2284ul);
        // EXPECT_EQ(state[0x29], pdp2_upper);
        EXPECT_EQ(state[0x2A], 0x2280ul);
        // EXPECT_EQ(state[0x2B], pdp2_lower);
        EXPECT_EQ(state[0x2C], 0x227Cul);
        // EXPECT_EQ(state[0x2D], pdp1_upper);
        EXPECT_EQ(state[0x2E], 0x2278ul);
        // EXPECT_EQ(state[0x2F], pdp1_lower);
        EXPECT_EQ(state[0x30], 0x2274ul);
        // EXPECT_EQ(state[0x31], pdp0_upper);
        EXPECT_EQ(state[0x32], 0x2270ul);
        // EXPECT_EQ(state[0x33], pdp0_lower);
        EXPECT_EQ(state[0x41], 0x11000001ul);
        EXPECT_EQ(state[0x42], 0x20C8ul);
        EXPECT_EQ(state[0x43], 0ul);
    }

    void InitHardware()
    {
        register_io()->Write32(
            engine_cs_->mmio_base() + registers::HardwareStatusPageAddress::kOffset, 0);
        register_io()->Write32(engine_cs_->mmio_base() + registers::GraphicsMode::kOffset, 0);

        engine_cs_->InitHardware(hw_status_page_.get());

        EXPECT_EQ(register_io()->Read32(engine_cs_->mmio_base() +
                                        registers::HardwareStatusPageAddress::kOffset),
                  mock_status_page_->gpu_addr);
        EXPECT_EQ(register_io()->Read32(engine_cs_->mmio_base() + registers::GraphicsMode::kOffset),
                  0x80008000u);

        EXPECT_EQ(hw_status_page_->read_sequence_number(), (uint32_t)kFirstSequenceNumber);
    }

    void RenderInit()
    {
        ASSERT_EQ(engine_cs_->id(), RENDER_COMMAND_STREAMER);

        InitContext();

        EXPECT_TRUE(context_->Map(address_space_.get(), engine_cs_->id()));

        auto ringbuffer = context_->get_ringbuffer(engine_cs_->id());
        ASSERT_NE(ringbuffer, nullptr);
        EXPECT_EQ(ringbuffer->tail(), 0u);

        auto render_cs = reinterpret_cast<RenderEngineCommandStreamer*>(engine_cs_.get());

        register_io_->enable_trace(true);

        EXPECT_TRUE(render_cs->RenderInit(context_.get()));

        EXPECT_EQ(ringbuffer->tail(), 0x24u);

        // Consider validating the content of the ring buffer here.

        gpu_addr_t gpu_addr;
        EXPECT_TRUE(ringbuffer->GetGpuAddress(address_space_->id(), &gpu_addr));

        void* addr;
        EXPECT_TRUE(TestContext::get_context_buffer(context_.get(), engine_cs_->id())
                        ->platform_buffer()
                        ->MapCpu(&addr));

        uint32_t* state = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(addr) + PAGE_SIZE);
        EXPECT_EQ(state[6], 0x2030ul);
        EXPECT_EQ(state[7], ringbuffer->tail());
        EXPECT_EQ(state[8], 0x2038ul);
        EXPECT_EQ(state[9], gpu_addr);

        EXPECT_TRUE(TestContext::get_context_buffer(context_.get(), engine_cs_->id())
                        ->platform_buffer()
                        ->UnmapCpu());

        EXPECT_TRUE(context_->GetGpuAddress(engine_cs_->id(), &gpu_addr));

        uint32_t upper_32_bits = gpu_addr >> 12;
        uint32_t lower_32_bits = gpu_addr | 0x29;
        std::vector<uint32_t> submitport_writes{0, 0, upper_32_bits, lower_32_bits};
        uint32_t write = 0;

        for (auto operation : register_io_->trace()) {
            if (operation.offset ==
                EngineCommandStreamer::kRenderEngineMmioBase +
                    registers::ExeclistSubmitPort::kSubmitOffset) {
                EXPECT_EQ(operation.type, RegisterIo::Operation::WRITE32);
                ASSERT_LT(write, submitport_writes.size());
                EXPECT_EQ(operation.val, submitport_writes[write++]);
            }
        }
        EXPECT_EQ(write, submitport_writes.size());

        EXPECT_TRUE(context_->Unmap(address_space_.get(), engine_cs_->id()));
    }

private:
    RegisterIo* register_io() override
    {
        return register_io_.get();
    }

    Sequencer* sequencer() override { return sequencer_.get(); }

    HardwareStatusPage* hardware_status_page(EngineCommandStreamerId id) override
    {
        return hw_status_page_.get();
    }

    void* hardware_status_page_cpu_addr(EngineCommandStreamerId id) override
    {
        EXPECT_EQ(id, engine_cs_->id());
        return mock_status_page_->cpu_addr;
    }

    gpu_addr_t hardware_status_page_gpu_addr(EngineCommandStreamerId id) override
    {
        EXPECT_EQ(id, engine_cs_->id());
        return mock_status_page_->gpu_addr;
    }

    std::unique_ptr<RegisterIo> register_io_;
    std::unique_ptr<AddressSpace> address_space_;
    std::unique_ptr<MsdIntelContext> context_;
    std::unique_ptr<MockStatusPageBuffer> mock_status_page_;
    std::unique_ptr<EngineCommandStreamer> engine_cs_;
    std::unique_ptr<Sequencer> sequencer_;
    std::unique_ptr<HardwareStatusPage> hw_status_page_;
};

TEST(RenderEngineCommandStreamer, InitContext)
{
    TestEngineCommandStreamer test;
    test.InitContext();
}

TEST(RenderEngineCommandStreamer, InitHardware)
{
    TestEngineCommandStreamer test;
    test.InitHardware();
}

TEST(RenderEngineCommandStreamer, RenderInit)
{
    TestEngineCommandStreamer test;
    test.RenderInit();
}
