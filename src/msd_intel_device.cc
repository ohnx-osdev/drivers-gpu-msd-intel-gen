// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "msd_intel_device.h"
#include "device_id.h"
#include "forcewake.h"
#include "global_context.h"
#include "magma_util/dlog.h"
#include "magma_util/macros.h"
#include "modeset/displayport.h"
#include "msd_intel_semaphore.h"
#include "registers.h"
#include <cstdio>
#include <string>

class MsdIntelDevice::CommandBufferRequest : public DeviceRequest {
public:
    CommandBufferRequest(std::unique_ptr<CommandBuffer> command_buffer)
        : command_buffer_(std::move(command_buffer))
    {
    }

protected:
    magma::Status Process(MsdIntelDevice* device) override
    {
        return device->ProcessCommandBuffer(std::move(command_buffer_));
    }

private:
    std::unique_ptr<CommandBuffer> command_buffer_;
};

class MsdIntelDevice::DestroyContextRequest : public DeviceRequest {
public:
    DestroyContextRequest(std::shared_ptr<ClientContext> client_context)
        : client_context_(std::move(client_context))
    {
    }

protected:
    magma::Status Process(MsdIntelDevice* device) override
    {
        return device->ProcessDestroyContext(std::move(client_context_));
    }

private:
    std::shared_ptr<ClientContext> client_context_;
};

class MsdIntelDevice::FlipRequest : public DeviceRequest {
public:
    FlipRequest(std::shared_ptr<MsdIntelBuffer> buffer, magma_system_image_descriptor* image_desc,
                std::vector<std::shared_ptr<magma::PlatformSemaphore>> wait_semaphores,
                std::vector<std::shared_ptr<magma::PlatformSemaphore>> signal_semaphores)
        : buffer_(std::move(buffer)), image_desc_(*image_desc),
          wait_semaphores_(std::move(wait_semaphores)),
          signal_semaphores_(std::move(signal_semaphores))
    {
    }

    // Takes ownership
    std::vector<std::shared_ptr<magma::PlatformSemaphore>> get_wait_semaphores()
    {
        return std::move(wait_semaphores_);
    }

protected:
    magma::Status Process(MsdIntelDevice* device) override
    {
        return device->ProcessFlip(buffer_, image_desc_, std::move(signal_semaphores_));
    }

private:
    std::shared_ptr<MsdIntelBuffer> buffer_;
    magma_system_image_descriptor image_desc_;
    std::vector<std::shared_ptr<magma::PlatformSemaphore>> wait_semaphores_;
    std::vector<std::shared_ptr<magma::PlatformSemaphore>> signal_semaphores_;
};

class MsdIntelDevice::InterruptRequest : public DeviceRequest {
public:
    InterruptRequest() {}

protected:
    magma::Status Process(MsdIntelDevice* device) override { return device->ProcessInterrupts(); }
};

//////////////////////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<MsdIntelDevice> MsdIntelDevice::Create(void* device_handle,
                                                       bool start_device_thread)
{
    std::unique_ptr<MsdIntelDevice> device(new MsdIntelDevice());

    if (!device->Init(device_handle))
        return DRETP(nullptr, "Failed to initialize MsdIntelDevice");

    if (start_device_thread)
        device->StartDeviceThread();

    return device;
}

MsdIntelDevice::MsdIntelDevice() { magic_ = kMagic; }

MsdIntelDevice::~MsdIntelDevice() { Destroy(); }

void MsdIntelDevice::Destroy()
{
    DLOG("Destroy");
    CHECK_THREAD_NOT_CURRENT(device_thread_id_);

    if (register_io_)
        registers::MasterInterruptControl::write(register_io_.get(), false);

    interrupt_thread_quit_flag_ = true;

    if (interrupt_)
        interrupt_->Close();

    if (interrupt_thread_.joinable()) {
        DLOG("joining interrupt thread");
        interrupt_thread_.join();
        DLOG("joined");
    }

    device_thread_quit_flag_ = true;

    if (monitor_)
        monitor_->Signal();

    if (device_thread_.joinable()) {
        DLOG("joining device thread");
        device_thread_.join();
        DLOG("joined");
    }
}

std::unique_ptr<MsdIntelConnection> MsdIntelDevice::Open(msd_client_id client_id)
{
    return MsdIntelConnection::Create(this, scratch_buffer_);
}

bool MsdIntelDevice::Init(void* device_handle)
{
    DASSERT(!platform_device_);

    DLOG("Init device_handle %p", device_handle);

    platform_device_ = magma::PlatformDevice::Create(device_handle);
    if (!platform_device_)
        return DRETF(false, "failed to create platform device");

    uint16_t pci_dev_id;
    if (!platform_device_->ReadPciConfig16(2, &pci_dev_id))
        return DRETF(false, "ReadPciConfig16 failed");

    device_id_ = pci_dev_id;
    DLOG("device_id 0x%x", device_id_);

    uint16_t gmch_graphics_ctrl;
    if (!platform_device_->ReadPciConfig16(registers::GmchGraphicsControl::kOffset,
                                           &gmch_graphics_ctrl))
        return DRETF(false, "ReadPciConfig16 failed");

    uint32_t gtt_size = registers::GmchGraphicsControl::gtt_size(gmch_graphics_ctrl);

    DLOG("gtt_size: %uMB", gtt_size >> 20);

    std::unique_ptr<magma::PlatformMmio> mmio(
        platform_device_->CpuMapPciMmio(0, magma::PlatformMmio::CACHE_POLICY_UNCACHED_DEVICE));
    if (!mmio)
        return DRETF(false, "failed to map pci bar 0");

    register_io_ = std::unique_ptr<RegisterIo>(new RegisterIo(std::move(mmio)));

    if (DeviceId::is_gen8(device_id_)) {
        ForceWake::reset(register_io_.get(), registers::ForceWake::GEN8);
        ForceWake::request(register_io_.get(), registers::ForceWake::GEN8);
    } else if (DeviceId::is_gen9(device_id_)) {
        ForceWake::reset(register_io_.get(), registers::ForceWake::GEN9_RENDER);
        ForceWake::request(register_io_.get(), registers::ForceWake::GEN9_RENDER);
    } else {
        DASSERT(false);
    }

    // Clear faults
    registers::AllEngineFault::clear(register_io_.get());

    interrupt_ = platform_device_->RegisterInterrupt();
    if (!interrupt_)
        return DRETF(false, "failed to register interrupt");

    PerProcessGtt::InitPrivatePat(register_io_.get());

    mapping_cache_ = GpuMappingCache::Create();

    gtt_ = std::make_shared<Gtt>(mapping_cache_);

    if (!gtt_->Init(gtt_size, platform_device_.get()))
        return DRETF(false, "failed to Init gtt");

    // Arbitrary
    constexpr uint32_t kFirstSequenceNumber = 0x1000;
    sequencer_ = std::unique_ptr<Sequencer>(new Sequencer(kFirstSequenceNumber));

    render_engine_cs_ = RenderEngineCommandStreamer::Create(this);

    global_context_ = std::shared_ptr<GlobalContext>(new GlobalContext(gtt_));

    // Creates the context backing store.
    if (!render_engine_cs_->InitContext(global_context_.get()))
        return DRETF(false, "render_engine_cs failed to init global context");

    if (!global_context_->Map(gtt_, render_engine_cs_->id()))
        return DRETF(false, "global context init failed");

    if (!RenderEngineInit())
        return DRETF(false, "failed to init render engine");

    monitor_ = magma::Monitor::CreateShared();

    semaphore_port_ = magma::SemaphorePort::Create();

    scratch_buffer_ =
        std::shared_ptr<magma::PlatformBuffer>(magma::PlatformBuffer::Create(PAGE_SIZE));

    if (!scratch_buffer_->PinPages(0, 1))
        return DRETF(false, "failed to pin pages scratch buffer");

    registers::MasterInterruptControl::write(register_io_.get(), true);

    return true;
}

bool MsdIntelDevice::RenderEngineInit()
{
    CHECK_THREAD_IS_CURRENT(device_thread_id_);

    progress_ = std::make_unique<GpuProgress>();

    render_engine_cs_->InitHardware();

    auto init_batch = render_engine_cs_->CreateRenderInitBatch(device_id_);
    if (!init_batch)
        return DRETF(false, "failed to create render init batch");

    if (!render_engine_cs_->RenderInit(global_context_, std::move(init_batch), gtt_))
        return DRETF(false, "render_engine_cs failed RenderInit");

    registers::MasterInterruptControl::write(register_io_.get(), true);

    // The modesetting code is only tested on gen 9 (Skylake).
    if (DeviceId::is_gen9(device_id_)) {
        // Eventually the modesetting code will be able to bring up a
        // display.  For now, all it does is fetch the display's EDID data.
        DisplayPort::FetchAndCheckEdidData(register_io_.get());
    }

    return true;
}

bool MsdIntelDevice::RenderEngineReset()
{
    magma::log(magma::LOG_WARNING, "resetting render engine");

    render_engine_cs_->ResetCurrentContext();

    registers::AllEngineFault::clear(register_io_.get());

    return RenderEngineInit();
}

void MsdIntelDevice::StartDeviceThread()
{
    DASSERT(!device_thread_.joinable());
    device_thread_ = std::thread([this] { this->DeviceThreadLoop(); });

    // TODO: move interrupt thread processing into device thread.
    // However for now, we need a separate interrupt thread and it
    // requires the device thread.
    DASSERT(!interrupt_thread_.joinable());
    interrupt_thread_ = std::thread([this] { this->InterruptThreadLoop(); });

    DASSERT(!wait_thread_.joinable());
    wait_thread_ = std::thread([this] { this->WaitThreadLoop(); });

    // TODO(MG-594): stop the wait thread like other threads
    wait_thread_.detach();
}

int MsdIntelDevice::InterruptThreadLoop()
{
    DLOG("Interrupt thread started");

    while (true) {
        DLOG("waiting for interrupt");
        interrupt_->Wait();
        DLOG("Returned from interrupt wait!");

        if (interrupt_thread_quit_flag_)
            break;

        auto request = std::make_unique<InterruptRequest>();
        auto reply = request->GetReply();

        EnqueueDeviceRequest(std::move(request), true);

        reply->Wait();

        interrupt_->Complete();
    }

    DLOG("Interrupt thread exited");
    return 0;
}

void MsdIntelDevice::WaitThreadLoop()
{
    DLOG("Wait thread started");

    while (semaphore_port_->WaitOne()) {
    }

    DLOG("Wait thread exited");
}

void MsdIntelDevice::Dump(DumpState* dump_out)
{
    dump_out->render_cs.sequence_number =
        global_context_->hardware_status_page(render_engine_cs_->id())->read_sequence_number();
    dump_out->render_cs.active_head_pointer = render_engine_cs_->GetActiveHeadPointer();

    DumpFault(dump_out, registers::AllEngineFault::read(register_io_.get()));

    dump_out->fault_gpu_address = kInvalidGpuAddr;
    if (dump_out->fault_present)
        DumpFaultAddress(dump_out, register_io_.get());
}

void MsdIntelDevice::DumpFault(DumpState* dump_out, uint32_t fault)
{
    dump_out->fault_present = registers::AllEngineFault::valid(fault);
    dump_out->fault_engine = registers::AllEngineFault::engine(fault);
    dump_out->fault_src = registers::AllEngineFault::src(fault);
    dump_out->fault_type = registers::AllEngineFault::type(fault);
}

void MsdIntelDevice::DumpFaultAddress(DumpState* dump_out, RegisterIo* register_io)
{
    dump_out->fault_gpu_address = registers::FaultTlbReadData::addr(register_io);
}

void MsdIntelDevice::DumpToString(std::string& dump_out)
{
    DumpState dump_state;
    Dump(&dump_state);

    const char* fmt = "---- device dump begin ----\n"
                      "Device id: 0x%x\n"
                      "RENDER_COMMAND_STREAMER\n"
                      "sequence_number 0x%x\n"
                      "active head pointer: 0x%llx\n";
    int size = std::snprintf(nullptr, 0, fmt, device_id(), dump_state.render_cs.sequence_number,
                             dump_state.render_cs.active_head_pointer);
    std::vector<char> buf(size + 1);
    std::snprintf(&buf[0], buf.size(), fmt, device_id(), dump_state.render_cs.sequence_number,
                  dump_state.render_cs.active_head_pointer);
    dump_out.append(&buf[0]);

    if (dump_state.fault_present) {
        fmt = "ENGINE FAULT DETECTED\n"
              "engine 0x%x src 0x%x type 0x%x gpu_address 0x%llx\n";
        size = std::snprintf(nullptr, 0, fmt, dump_state.fault_engine, dump_state.fault_src,
                             dump_state.fault_type, dump_state.fault_gpu_address);
        std::vector<char> buf(size + 1);
        std::snprintf(&buf[0], buf.size(), fmt, dump_state.fault_engine, dump_state.fault_src,
                      dump_state.fault_type);
        dump_out.append(&buf[0]);
    } else {
        dump_out.append("No engine faults detected.\n");
    }
    dump_out.append("---- device dump end ----");
}

magma::Status MsdIntelDevice::SubmitCommandBuffer(std::unique_ptr<CommandBuffer> command_buffer)
{
    DLOG("SubmitCommandBuffer");
    CHECK_THREAD_NOT_CURRENT(device_thread_id_);

    EnqueueDeviceRequest(std::make_unique<CommandBufferRequest>(std::move(command_buffer)));
    return MAGMA_STATUS_OK;
}

void MsdIntelDevice::DestroyContext(std::shared_ptr<ClientContext> client_context)
{
    DLOG("DestroyContext");
    CHECK_THREAD_NOT_CURRENT(device_thread_id_);

    EnqueueDeviceRequest(std::make_unique<DestroyContextRequest>(std::move(client_context)));
}

void MsdIntelDevice::Flip(std::shared_ptr<MsdIntelBuffer> buffer,
                          magma_system_image_descriptor* image_desc,
                          std::vector<std::shared_ptr<magma::PlatformSemaphore>> wait_semaphores,
                          std::vector<std::shared_ptr<magma::PlatformSemaphore>> signal_semaphores)
{
    DLOG("Flip buffer 0x%" PRIx64, buffer->platform_buffer()->id());

    CHECK_THREAD_NOT_CURRENT(device_thread_id_);
    DASSERT(buffer);

    auto request = std::make_unique<FlipRequest>(buffer, image_desc, std::move(wait_semaphores),
                                                 std::move(signal_semaphores));

    std::unique_lock<std::mutex> lock(pageflip_request_mutex_);
    pageflip_pending_queue_.push(std::move(request));

    if (pageflip_pending_queue_.size() == 1) {
        lock.unlock();
        ProcessPendingFlip();
    }
}

void MsdIntelDevice::ProcessPendingFlip()
{
    auto callback = [this](magma::SemaphorePort::WaitSet* wait_set) { this->ProcessPendingFlip(); };

    std::unique_lock<std::mutex> lock(pageflip_request_mutex_);

    while (pageflip_pending_queue_.size()) {
        DLOG("pageflip_pending_queue_ size %zu", pageflip_pending_queue_.size());

        std::unique_ptr<FlipRequest>& request = pageflip_pending_queue_.front();

        // Takes ownership
        auto semaphores = request->get_wait_semaphores();

        if (semaphores.size() == 0) {
            EnqueueDeviceRequest(std::move(request));
            pageflip_pending_queue_.pop();
        } else {
            DLOG("adding waitset with %zu semaphores", semaphores.size());
            // Invoke the callback when semaphores are satisfied;
            // the next ProcessPendingFlip will see an empty semaphore array for the front request.
            bool result = semaphore_port_->AddWaitSet(
                std::make_unique<magma::SemaphorePort::WaitSet>(callback, std::move(semaphores)));
            DASSERT(result);
            break;
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////

void MsdIntelDevice::EnqueueDeviceRequest(std::unique_ptr<DeviceRequest> request,
                                          bool enqueue_front)
{
    DASSERT(monitor_);
    magma::Monitor::Lock lock(monitor_);
    lock.Acquire();
    if (enqueue_front) {
        device_request_list_.emplace_front(std::move(request));
    } else {
        device_request_list_.emplace_back(std::move(request));
    }
    lock.Release();
    monitor_->Signal();
}

int MsdIntelDevice::DeviceThreadLoop()
{
    device_thread_id_ = std::make_unique<magma::PlatformThreadId>();
    CHECK_THREAD_IS_CURRENT(device_thread_id_);

    DLOG("DeviceThreadLoop starting thread 0x%x", device_thread_id_->id());

    DASSERT(monitor_);
    magma::Monitor::Lock lock(monitor_);
    lock.Acquire();

    constexpr uint32_t kTimeoutMs = 100;

    while (true) {
        if (progress_->work_outstanding()) {
            std::chrono::high_resolution_clock::time_point time_point =
                progress_->hangcheck_time_start() + std::chrono::milliseconds(kTimeoutMs + 1);
            DLOG("waiting with timeout");
            bool timed_out = false;
            monitor_->WaitUntil(&lock, time_point, &timed_out);
        } else {
            DLOG("waiting, no timeout");
            monitor_->Wait(&lock);
        }

        ProcessDeviceRequests(&lock);

        if (device_thread_quit_flag_)
            break;

        HangCheck(kTimeoutMs);
    }

    lock.Release();

    // Ensure gpu is idle
    render_engine_cs_->Reset();

    DLOG("DeviceThreadLoop exit");
    return 0;
}

void MsdIntelDevice::ProcessCompletedCommandBuffers()
{
    CHECK_THREAD_IS_CURRENT(device_thread_id_);

    uint32_t sequence_number =
        hardware_status_page(RENDER_COMMAND_STREAMER)->read_sequence_number();
    render_engine_cs_->ProcessCompletedCommandBuffers(sequence_number);

    progress_->Completed(sequence_number);
}

void MsdIntelDevice::ProcessDeviceRequests(magma::Monitor::Lock* lock)
{
    CHECK_THREAD_IS_CURRENT(device_thread_id_);

    if (lock)
        DASSERT(lock->acquired(monitor_.get()));

    while (device_request_list_.size()) {
        DLOG("device_request_list_.size() %zu", device_request_list_.size());

        auto request = std::move(device_request_list_.front());
        device_request_list_.pop_front();

        if (lock)
            lock->Release();

        DASSERT(request);
        request->ProcessAndReply(this);

        if (lock)
            lock->Acquire();
    }
}

magma::Status MsdIntelDevice::ProcessInterrupts()
{
    DLOG("ProcessInterrupts");

    uint32_t master_interrupt_control = registers::MasterInterruptControl::read(register_io_.get());

    registers::MasterInterruptControl::write(register_io_.get(), false);

    if (master_interrupt_control &
        registers::MasterInterruptControl::kRenderInterruptsPendingBitMask) {
        uint32_t val = registers::GtInterruptIdentity0::read(
            register_io(), registers::InterruptRegisterBase::RENDER_ENGINE);
        DLOG("gt IIR0 0x%08x", val);

        if (val & registers::InterruptRegisterBase::kUserInterruptBit) {
            registers::GtInterruptIdentity0::write(
                register_io(), registers::InterruptRegisterBase::RENDER_ENGINE,
                registers::InterruptRegisterBase::USER, registers::InterruptRegisterBase::MASK);

            bool fault = registers::AllEngineFault::read(register_io_.get()) &
                         registers::AllEngineFault::kValid;
            if (fault) {
                std::string s;
                DumpToString(s);
                magma::log(magma::LOG_WARNING, "GPU fault detected\n%s", s.c_str());
                RenderEngineReset();
            } else {
                ProcessCompletedCommandBuffers();
            }

        } else {
            DASSERT(false);
        }
    } else {
        DASSERT(false);
    }

    registers::MasterInterruptControl::write(register_io_.get(), true);

    return MAGMA_STATUS_OK;
}

void MsdIntelDevice::HangCheck(uint64_t timeout_ms)
{
    if (progress_->work_outstanding()) {
        std::chrono::duration<double, std::milli> elapsed =
            std::chrono::high_resolution_clock::now() - progress_->hangcheck_time_start();
        if (elapsed.count() >= timeout_ms) {
            std::string s;
            DumpToString(s);
            uint32_t master_interrupt_control =
                registers::MasterInterruptControl::read(register_io_.get());
            magma::log(magma::LOG_WARNING, "Suspected GPU hang: last submitted sequence number "
                                           "0x%x master_interrupt_control 0x%08x\n%s",
                       progress_->last_submitted_sequence_number(), master_interrupt_control,
                       s.c_str());
            RenderEngineReset();
        }
    }
}

magma::Status MsdIntelDevice::ProcessCommandBuffer(std::unique_ptr<CommandBuffer> command_buffer)
{
    CHECK_THREAD_IS_CURRENT(device_thread_id_);

    DLOG("preparing command buffer for execution");

    auto context = command_buffer->GetContext().lock();
    DASSERT(context);

    auto connection = context->connection().lock();
    if (connection && connection->context_killed())
        return DRET_MSG(MAGMA_STATUS_CONTEXT_KILLED, "Connection context killed");

    if (!command_buffer->PrepareForExecution(render_engine_cs_.get(), gtt()))
        return DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR,
                        "Failed to prepare command buffer for execution");

    render_engine_cs_->SubmitCommandBuffer(std::move(command_buffer));

    RequestMaxFreq();
    return MAGMA_STATUS_OK;
}

magma::Status MsdIntelDevice::ProcessDestroyContext(std::shared_ptr<ClientContext> client_context)
{
    DLOG("ProcessDestroyContext");
    CHECK_THREAD_IS_CURRENT(device_thread_id_);
    // Just let it go out of scope
    return MAGMA_STATUS_OK;
}

magma::Status MsdIntelDevice::ProcessFlip(
    std::shared_ptr<MsdIntelBuffer> buffer, const magma_system_image_descriptor& image_desc,
    std::vector<std::shared_ptr<magma::PlatformSemaphore>> signal_semaphores)
{
    CHECK_THREAD_IS_CURRENT(device_thread_id_);
    DASSERT(buffer);

    DLOG("ProcessFlip buffer 0x%" PRIx64, buffer->platform_buffer()->id());

    // Error indicators are passed to the callback
    magma::Status status(MAGMA_STATUS_OK);

    std::shared_ptr<GpuMapping> mapping =
        AddressSpace::GetSharedGpuMapping(gtt_, buffer, PAGE_SIZE);
    if (!mapping)
        return DRET_MSG(MAGMA_STATUS_MEMORY_ERROR, "Couldn't map buffer to gtt");

    uint32_t width, height;
    registers::DisplayPlaneSurfaceSize::read(
        register_io(), registers::DisplayPlaneSurfaceSize::PIPE_A_PLANE_1, &width, &height);

    // Controls whether the plane surface update happens immediately or on the next vblank.
    constexpr bool kUpdateOnVblank = true;

    // Controls whether we wait for the flip to complete.
    // Waiting for flip completion seems to imply waiting for the vsync/vblank as well.
    // Note, if not waiting for flip complete you need to be careful of mapping lifetime.
    // For simplicity we just maintain all display buffer mappings forever but we should
    // have the upper layers import/release display buffers.
    constexpr bool kWaitForFlip = true;

    registers::DisplayPlaneControl::enable_update_on_vblank(
        register_io(), registers::DisplayPlaneControl::PIPE_A_PLANE_1, kUpdateOnVblank);

    if (kWaitForFlip)
        registers::DisplayPipeInterrupt::update_mask_bits(
            register_io(), registers::DisplayPipeInterrupt::PIPE_A,
            registers::DisplayPipeInterrupt::kPlane1FlipDoneBit, true);

    constexpr uint32_t kCacheLineSize = 64;
    constexpr uint32_t kTileSize = 512;

    if (image_desc.tiling == MAGMA_IMAGE_TILING_OPTIMAL) {
        // Stride must be an integer number of tiles
        uint32_t stride = magma::round_up(width * sizeof(uint32_t), kTileSize) / kTileSize;
        registers::DisplayPlaneSurfaceStride::write(
            register_io(), registers::DisplayPlaneSurfaceStride::PIPE_A_PLANE_1, stride);

        registers::DisplayPlaneControl::set_tiling(register_io(),
                                                   registers::DisplayPlaneControl::PIPE_A_PLANE_1,
                                                   registers::DisplayPlaneControl::TILING_X);
    } else {
        // Stride must be an integer number of cache lines
        uint32_t stride =
            magma::round_up(width * sizeof(uint32_t), kCacheLineSize) / kCacheLineSize;
        registers::DisplayPlaneSurfaceStride::write(
            register_io(), registers::DisplayPlaneSurfaceStride::PIPE_A_PLANE_1, stride);

        registers::DisplayPlaneControl::set_tiling(register_io(),
                                                   registers::DisplayPlaneControl::PIPE_A_PLANE_1,
                                                   registers::DisplayPlaneControl::TILING_NONE);
    }

    registers::DisplayPlaneSurfaceAddress::write(
        register_io(), registers::DisplayPlaneSurfaceAddress::PIPE_A_PLANE_1, mapping->gpu_addr());

    if (kWaitForFlip) {
        constexpr uint32_t kRetryMsMax = 100;

        auto start = std::chrono::high_resolution_clock::now();

        while (true) {
            bool flip_done = false;

            registers::DisplayPipeInterrupt::process_identity_bits(
                register_io(), registers::DisplayPipeInterrupt::PIPE_A,
                registers::DisplayPipeInterrupt::kPlane1FlipDoneBit, &flip_done);
            if (flip_done)
                break;
            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double, std::milli> elapsed = end - start;
            if (elapsed.count() > kRetryMsMax)
                return DRET_MSG(MAGMA_STATUS_INTERNAL_ERROR, "Timeout waiting for page flip event");

            std::this_thread::yield();
        }

        registers::DisplayPipeInterrupt::update_mask_bits(
            register_io(), registers::DisplayPipeInterrupt::PIPE_A,
            registers::DisplayPipeInterrupt::kPlane1FlipDoneBit, false);
    }

    for (auto& semaphore : signal_semaphores_) {
        DLOG("signalling flip semaphore 0x%" PRIx64 "\n", semaphore->id());
        semaphore->Signal();
    }

    signal_semaphores_ = std::move(signal_semaphores);
    saved_display_mapping_ = std::move(mapping);

    return status;
}

bool MsdIntelDevice::WaitIdle()
{
    CHECK_THREAD_IS_CURRENT(device_thread_id_);

    if (!render_engine_cs_->WaitIdle()) {
        std::string s;
        DumpToString(s);
        printf("WaitRendering timed out!\n\n%s\n", s.c_str());
        return false;
    }
    return true;
}

void MsdIntelDevice::RequestMaxFreq()
{
    CHECK_THREAD_IS_CURRENT(device_thread_id_);

    uint32_t mhz = registers::RenderPerformanceStateCapability::read_rp0_frequency(register_io());
    registers::RenderPerformanceNormalFrequencyRequest::write_frequency_request_gen9(register_io(),
                                                                                     mhz);
}

uint32_t MsdIntelDevice::GetCurrentFrequency()
{
    CHECK_THREAD_IS_CURRENT(device_thread_id_);

    if (DeviceId::is_gen9(device_id_))
        return registers::RenderPerformanceStatus::read_current_frequency_gen9(register_io());

    DLOG("GetCurrentGraphicsFrequency not implemented");
    return 0;
}

HardwareStatusPage* MsdIntelDevice::hardware_status_page(EngineCommandStreamerId id)
{
    CHECK_THREAD_IS_CURRENT(device_thread_id_);
    DASSERT(global_context_);
    return global_context_->hardware_status_page(id);
}

//////////////////////////////////////////////////////////////////////////////////////////////////

msd_connection_t* msd_device_open(msd_device_t* dev, msd_client_id client_id)
{
    auto connection = MsdIntelDevice::cast(dev)->Open(client_id);
    if (!connection)
        return DRETP(nullptr, "MsdIntelDevice::Open failed");
    return new MsdIntelAbiConnection(std::move(connection));
}

void msd_device_destroy(msd_device_t* dev) { delete MsdIntelDevice::cast(dev); }

uint32_t msd_device_get_id(msd_device_t* dev) { return MsdIntelDevice::cast(dev)->device_id(); }

void msd_device_dump_status(struct msd_device_t* dev)
{
    std::string dump;
    MsdIntelDevice::cast(dev)->DumpToString(dump);
    printf("--------------------\n%s\n--------------------\n", dump.c_str());
}

void msd_device_page_flip(msd_device_t* dev, msd_buffer_t* buf,
                          magma_system_image_descriptor* image_desc, uint32_t wait_semaphore_count,
                          uint32_t signal_semaphore_count, msd_semaphore_t** semaphores)
{
    std::vector<std::shared_ptr<magma::PlatformSemaphore>> wait_semaphores(wait_semaphore_count);
    uint32_t index = 0;
    for (uint32_t i = 0; i < wait_semaphore_count; i++) {
        wait_semaphores[i] = MsdIntelAbiSemaphore::cast(semaphores[index++])->ptr();
    }
    std::vector<std::shared_ptr<magma::PlatformSemaphore>> signal_semaphores(
        signal_semaphore_count);
    for (uint32_t i = 0; i < signal_semaphore_count; i++) {
        signal_semaphores[i] = MsdIntelAbiSemaphore::cast(semaphores[index++])->ptr();
    }

    MsdIntelDevice::cast(dev)->Flip(MsdIntelAbiBuffer::cast(buf)->ptr(), image_desc,
                                    std::move(wait_semaphores), std::move(signal_semaphores));
}
