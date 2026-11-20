// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/nx2_bottleneck.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_scheduler_trace.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc_results.h"
#include "core/hle/kernel/svc_types.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"

namespace {

constexpr u64 kCodeAddr = 0x80000000ULL;
constexpr u64 kDataAddr = 0x80001000ULL;

constexpr u64 kWaiterEntry = kCodeAddr + 0x20;
constexpr u64 kSignalerEntry = kCodeAddr + 0x40;
constexpr u64 kSpinnerEntry = kCodeAddr + 0x80;

constexpr u64 kHandlesAddr = kDataAddr + 0x00;
constexpr u64 kWaitResultAddr = kDataAddr + 0x20;
constexpr u64 kWaitDoneAddr = kDataAddr + 0x28;
constexpr u64 kSignalResultAddr = kDataAddr + 0x30;
constexpr u64 kSpinFlagAddr = kDataAddr + 0x40;
constexpr u64 kSpinEnteredAddr = kDataAddr + 0x48;
constexpr u64 kMainStopAddr = kDataAddr + 0x50;

constexpr u64 kWaiterStackTop = kDataAddr + 0x800;
constexpr u64 kSignalerStackTop = kDataAddr + 0xA00;
constexpr u64 kSpinnerStackTop = kDataAddr + 0xC00;

// AArch64 encodings for minimal original guest stubs.
constexpr u32 kSvcExitThread = 0xD4000141;          // svc #0xa
constexpr u32 kSvcSleepThread = 0xD4000161;         // svc #0xb
constexpr u32 kSvcSignalEvent = 0xD4000221;         // svc #0x11
constexpr u32 kSvcWaitSynchronization = 0xD4000301; // svc #0x18
constexpr u32 kMovX19CodeBase = 0xD2B00013;         // movz x19, #0x8000, lsl #16
constexpr u32 kLdrW5X19Off1050 = 0xB9505265;        // ldr w5, [x19, #0x1050]
constexpr u32 kCbnzW5Plus16 = 0x35000085;           // cbnz w5, #+16
constexpr u32 kMovX0Zero = 0xD2800000;              // movz x0, #0
constexpr u32 kBMinus16 = 0x17FFFFFC;               // b #-16
constexpr u32 kStrW0X19 = 0xB9000260;               // str w0, [x19]
constexpr u32 kMovW4One = 0x52800024;               // mov w4, #1
constexpr u32 kDmbSy = 0xD5033FBF;                  // dmb sy
constexpr u32 kStrW4X20 = 0xB9000284;               // str w4, [x20]
constexpr u32 kLdrW5X19 = 0xB9400265;               // ldr w5, [x19]
constexpr u32 kCbzW5Back1 = 0x34FFFFE5;             // cbz w5, #-4

void WriteInsn(std::vector<u8>& mem, u64 offset, u32 insn) {
    std::memcpy(mem.data() + offset, &insn, sizeof(insn));
}

Kernel::CodeSet BuildControlCodeSet() {
    Kernel::CodeSet code_set;
    code_set.memory.resize(0x2000, 0);

    // Keep-alive anchor at offset 0x00 for KProcess::Run's initial thread so the process does not
    // auto-terminate its handle table before worker/waiter threads are created.
    WriteInsn(code_set.memory, 0x00, kMovX19CodeBase);
    WriteInsn(code_set.memory, 0x04, kLdrW5X19Off1050);
    WriteInsn(code_set.memory, 0x08, kCbnzW5Plus16);
    WriteInsn(code_set.memory, 0x0C, kMovX0Zero);
    WriteInsn(code_set.memory, 0x10, kSvcSleepThread);
    WriteInsn(code_set.memory, 0x14, kBMinus16);
    WriteInsn(code_set.memory, 0x18, kSvcExitThread);

    // Waiter: WaitSynchronization(out, handles=X1, count=X2, timeout=X3); store W0 -> [X19];
    // dmb sy; store 1 -> [X20]; ExitThread.
    WriteInsn(code_set.memory, 0x20, kSvcWaitSynchronization);
    WriteInsn(code_set.memory, 0x24, kStrW0X19);
    WriteInsn(code_set.memory, 0x28, kMovW4One);
    WriteInsn(code_set.memory, 0x2C, kDmbSy);
    WriteInsn(code_set.memory, 0x30, kStrW4X20);
    WriteInsn(code_set.memory, 0x34, kSvcExitThread);

    // Signaler: SignalEvent(handle=X0); store W0 -> [X19]; ExitThread.
    WriteInsn(code_set.memory, 0x40, kSvcSignalEvent);
    WriteInsn(code_set.memory, 0x44, kStrW0X19);
    WriteInsn(code_set.memory, 0x48, kSvcExitThread);

    // Spinner: store 1 -> [X20]; loop reading [X19] until nonzero; ExitThread.
    WriteInsn(code_set.memory, 0x80, kMovW4One);
    WriteInsn(code_set.memory, 0x84, kDmbSy);
    WriteInsn(code_set.memory, 0x88, kStrW4X20);
    WriteInsn(code_set.memory, 0x8C, kLdrW5X19);
    WriteInsn(code_set.memory, 0x90, kCbzW5Back1);
    WriteInsn(code_set.memory, 0x94, kSvcExitThread);

    code_set.CodeSegment().addr = 0;
    code_set.CodeSegment().offset = 0;
    code_set.CodeSegment().size = 0x1000;

    code_set.RODataSegment().addr = 0x1000;
    code_set.RODataSegment().offset = 0x1000;
    code_set.RODataSegment().size = 0;

    code_set.DataSegment().addr = 0x1000;
    code_set.DataSegment().offset = 0x1000;
    code_set.DataSegment().size = 0x1000;

    return code_set;
}

std::vector<u32> BuildProcessCapabilities() {
    // CorePriority: cores 0..5, priorities 16..63.
    const u32 core_prio = 0x7U | (63U << 4) | (16U << 10) | (0U << 16) | (5U << 24);
    // Syscall masks for indices 0, 1, 2 (covering SVCs 0x00..0x47).
    const u32 svc0 = 0xFU | (0xFFFFFFU << 5) | (0U << 29);
    const u32 svc1 = 0xFU | (0xFFFFFFU << 5) | (1U << 29);
    const u32 svc2 = 0xFU | (0xFFFFFFU << 5) | (2U << 29);
    // KernelVersion 16.0.
    const u32 kver = 0x3FFFU | (0U << 15) | (16U << 19);
    // HandleTable size 64.
    const u32 htab = 0x7FFFU | (64U << 16);
    return {core_prio, svc0, svc1, svc2, kver, htab};
}

struct KernelRuntime {
    KernelRuntime() {
        Settings::values.use_multi_core.SetValue(true);
        Settings::values.memory_layout_mode.SetValue(Settings::MemoryLayout::Memory_4Gb);
        system.Initialize();
        auto& kernel = system.Kernel();
        kernel.Initialize();
        system.GetCpuManager().Initialize();
        system.GetCpuManager().OnGpuReady();
        system.CoreTiming().SyncPause(false);
    }

    ~KernelRuntime() {
        auto& kernel = system.Kernel();
        system.SetShuttingDown(true);
        kernel.RequestSuspendEmulation();
        system.CoreTiming().SyncPause(false);
        kernel.SuspendEmulation(true);
        kernel.SetApplicationProcess(nullptr);
        kernel.ShutdownCores();
        Nx2Bottleneck::ClearSleepThreadCounters();
        system.CoreTiming().ClearPendingEvents();
        system.GetCpuManager().Shutdown();
        kernel.Shutdown();
    }

    ScopedSystemTestContext ctx;
    Core::System system;
};

template <typename Fn>
void RunInFreshKernelThread(Fn&& fn) {
    std::exception_ptr err;
    std::jthread worker([&] {
        try {
            fn();
        } catch (...) {
            err = std::current_exception();
        }
    });
    worker.join();
    if (err) {
        std::rethrow_exception(err);
    }
}

class MulticoreKernelHarness {
public:
    MulticoreKernelHarness() {
        auto& kernel = system.Kernel();

        process = Kernel::KProcess::Create(kernel);
        REQUIRE(process != nullptr);
        Kernel::KProcess::Register(kernel, process);
        kernel.AppendNewProcess(process);

        const auto phys_mem_size =
            kernel.MemoryManager().GetSize(Kernel::KMemoryManager::Pool::Application);
        auto* res_limit = Kernel::CreateResourceLimitForProcess(system, phys_mem_size);
        REQUIRE(res_limit != nullptr);
        SCOPE_EXIT {
            res_limit->Close(kernel);
        };

        Kernel::Svc::CreateProcessParameter params{};
        constexpr std::array<char, 12> kName{'s', 'c', 'h', 'e', 'd', '_', 'c', 't', 'r', 'l', 0, 0};
        params.name = kName;
        params.program_id = 0x1000;
        params.code_address = kCodeAddr;
        params.code_num_pages = 2;
        params.flags = Kernel::Svc::CreateProcessFlag::AddressSpace64Bit |
                       Kernel::Svc::CreateProcessFlag::Is64Bit |
                       Kernel::Svc::CreateProcessFlag::IsApplication;

        const auto caps = BuildProcessCapabilities();
        const auto init_res =
            process->Initialize(kernel, params, caps, res_limit,
                                Kernel::KMemoryManager::Pool::Application, 0);
        INFO("KProcess::Initialize failed with result 0x" << std::hex << init_res.GetInnerValue());
        REQUIRE(init_res.IsSuccess());
        process->LoadModule(kernel, BuildControlCodeSet(), kCodeAddr);
        process->InitializeInterfaces(kernel);
        kernel.SetApplicationProcess(process);
        REQUIRE(process->Run(kernel, 60, 0x1000).IsSuccess());
    }

    ~MulticoreKernelHarness() {
        auto& kernel = system.Kernel();
        if (process != nullptr) {
            process->GetMemory().Write32(kMainStopAddr, 1);
            kernel.SetApplicationProcess(nullptr);
            process->Close(kernel);
            process = nullptr;
        } else {
            kernel.SetApplicationProcess(nullptr);
        }
    }

    Kernel::KThread* SpawnUserThread(u64 entry, u64 stack_top, s32 priority, s32 core_id,
                                     u64 affinity_mask) {
        auto& kernel = system.Kernel();
        Kernel::KScopedResourceReservation thread_res{kernel, process,
                                                      Kernel::LimitableResource::ThreadCountMax};
        REQUIRE(thread_res.Succeeded());
        auto* thread = Kernel::KThread::Create(kernel);
        REQUIRE(thread != nullptr);
        REQUIRE(Kernel::KThread::InitializeUserThread(system, thread, entry, 0, stack_top, priority,
                                                    core_id, process)
                    .IsSuccess());
        thread_res.Commit();
        Kernel::KThread::Register(kernel, thread);
        REQUIRE(thread->SetCoreMask(kernel, core_id, affinity_mask).IsSuccess());
        return thread;
    }

    template <typename Pred>
    bool WaitUntil(Pred&& pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!pred()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        return true;
    }

    KernelRuntime runtime;
    Core::System& system{runtime.system};
    Kernel::KProcess* process{};
};

} // namespace

TEST_CASE("Scheduler trace: cross-core signaled wait joins wait-signal-runnable-dispatch-guest",
          "[core][sched_trace]") {
    RunInFreshKernelThread([] {
    MulticoreKernelHarness h;
    auto& kernel = h.system.Kernel();
    kernel.SetSchedulerTraceEnabled(true);

    auto* ev = Kernel::KEvent::Create(kernel);
    REQUIRE(ev != nullptr);
    SCOPE_EXIT {
        ev->Close(kernel);
    };
    ev->Initialize(kernel, h.process);
    Kernel::KEvent::Register(kernel, ev);

    Kernel::Handle ev_handle{};
    REQUIRE(h.process->GetHandleTable().Add(kernel, &ev_handle, &ev->GetReadableEvent()).IsSuccess());
    SCOPE_EXIT {
        h.process->GetHandleTable().Remove(kernel, ev_handle);
    };

    Kernel::Handle ev_write_handle{};
    REQUIRE(h.process->GetHandleTable().Add(kernel, &ev_write_handle, ev).IsSuccess());
    SCOPE_EXIT {
        h.process->GetHandleTable().Remove(kernel, ev_write_handle);
    };

    auto& mem = h.process->GetMemory();
    mem.Write32(kHandlesAddr, ev_handle);
    mem.Write32(kWaitResultAddr, 0xDEADBEEF);
    mem.Write32(kWaitDoneAddr, 0);
    mem.Write32(kSignalResultAddr, 0xDEADBEEF);

    // Waiter pinned to core 5 via affinity mask (1 << 5).
    auto* waiter = h.SpawnUserThread(kWaiterEntry, kWaiterStackTop, 44, 5, 1ULL << 5);
    SCOPE_EXIT {
        waiter->Close(kernel);
    };
    waiter->GetContext().r[1] = kHandlesAddr;
    waiter->GetContext().r[2] = 1;
    waiter->GetContext().r[3] = static_cast<u64>(-1); // infinite wait
    waiter->GetContext().r[19] = kWaitResultAddr;
    waiter->GetContext().r[20] = kWaitDoneAddr;

    REQUIRE(waiter->Run(kernel).IsSuccess());

    // Wait until the waiter has entered WaitBegin on the trace (and parked in Waiting).
    REQUIRE(h.WaitUntil([&] {
        const auto snap = kernel.SchedulerTrace().Snapshot();
        for (const auto& rec : snap.records) {
            if (rec.kind == Kernel::KSchedulerTrace::EventKind::WaitBegin &&
                rec.thread_key == Kernel::KSchedulerTrace::ThreadKey(waiter)) {
                return true;
            }
        }
        return false;
    }));
    // Brief settle so core 5 finishes transitioning into Waiting/Idle before signal.
    REQUIRE(h.WaitUntil([&] {
        return waiter->GetState() == Kernel::ThreadState::Waiting;
    }));

    // Signaler runs on core 0 and signals the event across cores.
    auto* signaler = h.SpawnUserThread(kSignalerEntry, kSignalerStackTop, 44, 0, 1ULL << 0);
    SCOPE_EXIT {
        signaler->Close(kernel);
    };
    signaler->GetContext().r[0] = ev_write_handle;
    signaler->GetContext().r[19] = kSignalResultAddr;
    REQUIRE(signaler->Run(kernel).IsSuccess());

    // Wait for guest continuation acknowledgement from the waiter.
    REQUIRE(h.WaitUntil([&] { return mem.Read32(kWaitDoneAddr) == 1; }));
    REQUIRE(mem.Read32(kWaitResultAddr) == ResultSuccess.GetInnerValue());
    REQUIRE(mem.Read32(kSignalResultAddr) == ResultSuccess.GetInnerValue());

    // Ensure both threads have finished exiting.
    REQUIRE(h.WaitUntil([&] {
        return waiter->GetState() == Kernel::ThreadState::Terminated &&
               signaler->GetState() == Kernel::ThreadState::Terminated;
    }));

    const auto snap = kernel.SchedulerTrace().Snapshot();
    const auto analysis = Kernel::KSchedulerTrace::Analyze(snap);
    INFO(Kernel::KSchedulerTrace::FormatSummary(analysis));

    CHECK(analysis.completed_wakes >= 1);
    CHECK(analysis.missed_wakes == 0);
    CHECK(analysis.runnable_unscheduled == 0);
    CHECK(analysis.handoff_without_guest_enter == 0);
    CHECK(analysis.signal_to_runnable_virt.count >= 1);
    CHECK(analysis.runnable_to_handoff_virt.count >= 1);
    CHECK(analysis.handoff_to_guest_virt.count >= 1);
    CHECK(analysis.runnable_to_running_virt.count >= 1);
    CHECK(analysis.runnable_to_running_host.count >= 1);
    });
}

TEST_CASE("Scheduler trace: unsignaled timeout negative control separates timer expiry from signal",
          "[core][sched_trace]") {
    RunInFreshKernelThread([] {
    MulticoreKernelHarness h;
    auto& kernel = h.system.Kernel();
    kernel.SetSchedulerTraceEnabled(true);

    auto* ev = Kernel::KEvent::Create(kernel);
    REQUIRE(ev != nullptr);
    SCOPE_EXIT {
        ev->Close(kernel);
    };
    ev->Initialize(kernel, h.process);
    Kernel::KEvent::Register(kernel, ev);

    Kernel::Handle ev_handle{};
    REQUIRE(h.process->GetHandleTable().Add(kernel, &ev_handle, &ev->GetReadableEvent()).IsSuccess());
    SCOPE_EXIT {
        h.process->GetHandleTable().Remove(kernel, ev_handle);
    };

    auto& mem = h.process->GetMemory();
    mem.Write32(kHandlesAddr, ev_handle);
    mem.Write32(kWaitResultAddr, 0xDEADBEEF);
    mem.Write32(kWaitDoneAddr, 0);

    auto* waiter = h.SpawnUserThread(kWaiterEntry, kWaiterStackTop, 44, 5, 1ULL << 5);
    SCOPE_EXIT {
        waiter->Close(kernel);
    };
    waiter->GetContext().r[1] = kHandlesAddr;
    waiter->GetContext().r[2] = 1;
    waiter->GetContext().r[3] = 2'000'000ULL; // 2 ms timeout, never signaled
    waiter->GetContext().r[19] = kWaitResultAddr;
    waiter->GetContext().r[20] = kWaitDoneAddr;

    REQUIRE(waiter->Run(kernel).IsSuccess());
    REQUIRE(h.WaitUntil([&] { return mem.Read32(kWaitDoneAddr) == 1; }));
    REQUIRE(mem.Read32(kWaitResultAddr) == Kernel::ResultTimedOut.GetInnerValue());
    REQUIRE(h.WaitUntil([&] { return waiter->GetState() == Kernel::ThreadState::Terminated; }));

    const auto snap = kernel.SchedulerTrace().Snapshot();
    const auto analysis = Kernel::KSchedulerTrace::Analyze(snap);
    INFO(Kernel::KSchedulerTrace::FormatSummary(analysis));

    CHECK(analysis.completed_timed_out >= 1);
    CHECK(analysis.missed_wakes == 0);
    CHECK(analysis.runnable_unscheduled == 0);

    for (const auto& rec : snap.records) {
        if (rec.kind == Kernel::KSchedulerTrace::EventKind::Signal &&
            rec.thread_key == Kernel::KSchedulerTrace::ThreadKey(waiter)) {
            CHECK(rec.sub_kind ==
                  static_cast<u8>(Kernel::KSchedulerTrace::SignalOutcome::TimerExpired));
        }
    }
    });
}

TEST_CASE("Scheduler trace: contended spin isolates runnable-unscheduled from missed-wake",
          "[core][sched_trace]") {
    RunInFreshKernelThread([] {
    MulticoreKernelHarness h;
    auto& kernel = h.system.Kernel();
    kernel.SetSchedulerTraceEnabled(true);

    auto* ev = Kernel::KEvent::Create(kernel);
    REQUIRE(ev != nullptr);
    SCOPE_EXIT {
        ev->Close(kernel);
    };
    ev->Initialize(kernel, h.process);
    Kernel::KEvent::Register(kernel, ev);

    Kernel::Handle ev_handle{};
    REQUIRE(h.process->GetHandleTable().Add(kernel, &ev_handle, &ev->GetReadableEvent()).IsSuccess());
    SCOPE_EXIT {
        h.process->GetHandleTable().Remove(kernel, ev_handle);
    };

    Kernel::Handle ev_write_handle{};
    REQUIRE(h.process->GetHandleTable().Add(kernel, &ev_write_handle, ev).IsSuccess());
    SCOPE_EXIT {
        h.process->GetHandleTable().Remove(kernel, ev_write_handle);
    };

    auto& mem = h.process->GetMemory();
    mem.Write32(kHandlesAddr, ev_handle);
    mem.Write32(kWaitResultAddr, 0xDEADBEEF);
    mem.Write32(kWaitDoneAddr, 0);
    mem.Write32(kSpinFlagAddr, 0);
    mem.Write32(kSpinEnteredAddr, 0);

    // 1) Start waiter on core 5 (priority 44, affinity = core 5 only), let it enter Waiting.
    auto* waiter = h.SpawnUserThread(kWaiterEntry, kWaiterStackTop, 44, 5, 1ULL << 5);
    SCOPE_EXIT {
        waiter->Close(kernel);
    };
    waiter->GetContext().r[1] = kHandlesAddr;
    waiter->GetContext().r[2] = 1;
    waiter->GetContext().r[3] = static_cast<u64>(-1);
    waiter->GetContext().r[19] = kWaitResultAddr;
    waiter->GetContext().r[20] = kWaitDoneAddr;
    REQUIRE(waiter->Run(kernel).IsSuccess());
    REQUIRE(h.WaitUntil([&] { return waiter->GetState() == Kernel::ThreadState::Waiting; }));

    // 2) Start spinner on core 5 at priority 44 (not rotated by 10 ms preemption).
    auto* spinner = h.SpawnUserThread(kSpinnerEntry, kSpinnerStackTop, 44, 5, 1ULL << 5);
    SCOPE_EXIT {
        mem.Write32(kSpinFlagAddr, 1);
        spinner->Close(kernel);
    };
    spinner->GetContext().r[19] = kSpinFlagAddr;
    spinner->GetContext().r[20] = kSpinEnteredAddr;
    REQUIRE(spinner->Run(kernel).IsSuccess());
    REQUIRE(h.WaitUntil([&] { return mem.Read32(kSpinEnteredAddr) == 1; }));

    // 3) Signal the event from core 0 while core 5 is actively running spinner in guest JIT.
    auto* signaler = h.SpawnUserThread(kSignalerEntry, kSignalerStackTop, 44, 0, 1ULL << 0);
    SCOPE_EXIT {
        signaler->Close(kernel);
    };
    signaler->GetContext().r[0] = ev_write_handle;
    signaler->GetContext().r[19] = kSignalResultAddr;
    REQUIRE(signaler->Run(kernel).IsSuccess());
    REQUIRE(h.WaitUntil([&] { return signaler->GetState() == Kernel::ThreadState::Terminated; }));

    // Wait until the waiter has transitioned to Runnable in the trace, while still blocked behind spinner.
    REQUIRE(h.WaitUntil([&] {
        const auto snap = kernel.SchedulerTrace().Snapshot();
        for (const auto& rec : snap.records) {
            if (rec.kind == Kernel::KSchedulerTrace::EventKind::Runnable &&
                rec.thread_key == Kernel::KSchedulerTrace::ThreadKey(waiter)) {
                return true;
            }
        }
        return false;
    }));
    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    // Waiter must still not have completed because spinner holds core 5.
    CHECK(mem.Read32(kWaitDoneAddr) == 0);

    const auto contended_snap = kernel.SchedulerTrace().Snapshot();
    const auto contended = Kernel::KSchedulerTrace::Analyze(contended_snap);
    INFO(Kernel::KSchedulerTrace::FormatSummary(contended));

    CHECK(contended.missed_wakes == 0);
    CHECK(contended.runnable_unscheduled >= 1);
    CHECK(contended.guest_spin_suspects >= 1);

    // 4) Release spinner and verify waiter completes cleanly.
    mem.Write32(kSpinFlagAddr, 1);
    REQUIRE(h.WaitUntil([&] { return mem.Read32(kWaitDoneAddr) == 1; }));
    REQUIRE(mem.Read32(kWaitResultAddr) == ResultSuccess.GetInnerValue());
    REQUIRE(h.WaitUntil([&] {
        return waiter->GetState() == Kernel::ThreadState::Terminated &&
               spinner->GetState() == Kernel::ThreadState::Terminated;
    }));

    const auto final_analysis = Kernel::KSchedulerTrace::Analyze(kernel.SchedulerTrace().Snapshot());
    CHECK(final_analysis.runnable_unscheduled == 0);
    CHECK(final_analysis.missed_wakes == 0);
    CHECK(final_analysis.completed_wakes >= 1);
    });
}

TEST_CASE("Scheduler trace analyzer distinguishes missed wake, mutex transfer, ordinary wait, and handoff gap",
          "[core][sched_trace]") {
    using Trace = Kernel::KSchedulerTrace;

    // 1) Missed wake: WaitBegin + WakeReady signal without WaitOutcome or Runnable.
    {
        Trace::SnapshotResult snap;
        snap.records.push_back({
            .seq = 1,
            .virt_ns = 1000,
            .host_ns = 1000,
            .thread_key = 0xCAFE,
            .wait_id = 1,
            .object_key = 0xBEEF,
            .emitter_core = 5,
            .subject_core = 5,
            .priority = 44,
            .affinity_mask = 0x20,
            .kind = Trace::EventKind::WaitBegin,
            .sub_kind = static_cast<u8>(Trace::WaitKind::Synchronization),
        });
        snap.records.push_back({
            .seq = 2,
            .virt_ns = 2000,
            .host_ns = 2000,
            .thread_key = 0xCAFE,
            .wait_id = 1,
            .object_key = 0xBEEF,
            .emitter_core = 0,
            .subject_core = 5,
            .priority = 44,
            .affinity_mask = 0x20,
            .kind = Trace::EventKind::Signal,
            .sub_kind = static_cast<u8>(Trace::SignalOutcome::WakeReady),
        });
        snap.total_written = 2;
        const auto a = Trace::Analyze(snap);
        CHECK(a.missed_wakes == 1);
        CHECK(a.ordinary_waits == 0);
        CHECK(a.runnable_unscheduled == 0);
        REQUIRE(a.findings.size() == 1);
        CHECK(a.findings[0].kind == Trace::FindingKind::MissedWake);
        CHECK(a.findings[0].thread_alias == "t1");
        CHECK(a.findings[0].object_alias == "obj1");
    }

    // 2) Mutex transfer on condition variable signal is NOT a missed wake (ordinary wait on mutex).
    {
        Trace::SnapshotResult snap;
        snap.records.push_back({
            .seq = 1,
            .virt_ns = 1000,
            .host_ns = 1000,
            .thread_key = 101,
            .wait_id = 1,
            .object_key = 202,
            .emitter_core = 5,
            .subject_core = 5,
            .priority = 44,
            .affinity_mask = 0x20,
            .kind = Trace::EventKind::WaitBegin,
            .sub_kind = static_cast<u8>(Trace::WaitKind::ConditionVar),
        });
        snap.records.push_back({
            .seq = 2,
            .virt_ns = 2000,
            .host_ns = 2000,
            .thread_key = 101,
            .wait_id = 1,
            .object_key = 202,
            .secondary_thread_key = 102,
            .emitter_core = 0,
            .subject_core = 5,
            .priority = 44,
            .affinity_mask = 0x20,
            .kind = Trace::EventKind::Signal,
            .sub_kind = static_cast<u8>(Trace::SignalOutcome::MovedToMutex),
        });
        snap.total_written = 2;
        const auto a = Trace::Analyze(snap);
        CHECK(a.missed_wakes == 0);
        CHECK(a.ordinary_waits == 1);
    }

    // 3) Handoff without GuestEnter is reported separately from RunnableUnscheduled and MissedWake.
    {
        Trace::SnapshotResult snap;
        snap.records.push_back({
            .seq = 1,
            .virt_ns = 1000,
            .host_ns = 1000,
            .thread_key = 101,
            .wait_id = 1,
            .emitter_core = 5,
            .subject_core = 5,
            .priority = 44,
            .affinity_mask = 0x20,
            .kind = Trace::EventKind::Runnable,
        });
        snap.records.push_back({
            .seq = 2,
            .virt_ns = 1500,
            .host_ns = 1500,
            .thread_key = 101,
            .wait_id = 1,
            .handoff_gen = 7,
            .emitter_core = 5,
            .subject_core = 5,
            .priority = 44,
            .affinity_mask = 0x20,
            .kind = Trace::EventKind::Handoff,
        });
        snap.total_written = 2;
        const auto a = Trace::Analyze(snap);
        CHECK(a.missed_wakes == 0);
        CHECK(a.runnable_unscheduled == 0);
        CHECK(a.handoff_without_guest_enter == 1);
    }
}

TEST_CASE("Scheduler trace bounded ring, anonymization, and logging overhead control",
          "[core][sched_trace]") {
    // 1) Bounded capacity & anonymization check.
    Kernel::KSchedulerTrace trace;
    trace.SetEnabled(true, 32);
    for (u64 i = 0; i < 100; ++i) {
        trace.Record({
            .virt_ns = 1000 + i * 10,
            .thread_key = 0x11223344ULL,
            .wait_id = i + 1,
            .object_key = 0x55667788ULL,
            .emitter_core = 5,
            .subject_core = 5,
            .priority = 44,
            .affinity_mask = 0x20,
            .kind = Kernel::KSchedulerTrace::EventKind::WaitBegin,
        });
    }
    const auto snap = trace.Snapshot();
    const auto analysis = Kernel::KSchedulerTrace::Analyze(snap);
    CHECK(analysis.total_records == 100);
    CHECK(analysis.retained_records == 32);
    CHECK(analysis.overwritten_records == 68);
    CHECK(analysis.buffer_bytes ==
          Kernel::KSchedulerTrace::NumProducers * 32 *
              sizeof(Kernel::KSchedulerTrace::TransitionRecord));

    const std::string summary = Kernel::KSchedulerTrace::FormatSummary(analysis);
    CHECK(summary.find("11223344") == std::string::npos);
    CHECK(summary.find("55667788") == std::string::npos);
    REQUIRE(!analysis.findings.empty());
    CHECK(analysis.findings[0].thread_alias == "t1");
    CHECK(analysis.findings[0].object_alias == "obj1");

    // 2) Paired trace-off vs trace-on progressing control overhead measurement.
    constexpr int kRounds = 12;
    auto run_rounds = [&](bool trace_on) -> std::pair<double, Kernel::KSchedulerTrace::TraceAnalysis> {
        std::pair<double, Kernel::KSchedulerTrace::TraceAnalysis> result{};
        RunInFreshKernelThread([&] {
        MulticoreKernelHarness h;
        auto& kernel = h.system.Kernel();
        kernel.SetSchedulerTraceEnabled(trace_on);

        auto* ev = Kernel::KEvent::Create(kernel);
        REQUIRE(ev != nullptr);
        SCOPE_EXIT {
            ev->Close(kernel);
        };
        ev->Initialize(kernel, h.process);
        Kernel::KEvent::Register(kernel, ev);

        Kernel::Handle ev_handle{};
        REQUIRE(h.process->GetHandleTable()
                    .Add(kernel, &ev_handle, &ev->GetReadableEvent())
                    .IsSuccess());
        SCOPE_EXIT {
            h.process->GetHandleTable().Remove(kernel, ev_handle);
        };

        Kernel::Handle ev_write_handle{};
        REQUIRE(h.process->GetHandleTable().Add(kernel, &ev_write_handle, ev).IsSuccess());
        SCOPE_EXIT {
            h.process->GetHandleTable().Remove(kernel, ev_write_handle);
        };

        auto& mem = h.process->GetMemory();
        mem.Write32(kHandlesAddr, ev_handle);

        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < kRounds; ++r) {
            REQUIRE(ev->Clear(kernel).IsSuccess());
            mem.Write32(kWaitResultAddr, 0xDEADBEEF);
            mem.Write32(kWaitDoneAddr, 0);

            auto* waiter = h.SpawnUserThread(kWaiterEntry, kWaiterStackTop, 44, 5, 1ULL << 5);
            SCOPE_EXIT {
                waiter->Close(kernel);
            };
            waiter->GetContext().r[1] = kHandlesAddr;
            waiter->GetContext().r[2] = 1;
            waiter->GetContext().r[3] = static_cast<u64>(-1);
            waiter->GetContext().r[19] = kWaitResultAddr;
            waiter->GetContext().r[20] = kWaitDoneAddr;
            REQUIRE(waiter->Run(kernel).IsSuccess());
            REQUIRE(h.WaitUntil([&] { return waiter->GetState() == Kernel::ThreadState::Waiting; }));

            auto* signaler = h.SpawnUserThread(kSignalerEntry, kSignalerStackTop, 44, 0, 1ULL << 0);
            SCOPE_EXIT {
                signaler->Close(kernel);
            };
            signaler->GetContext().r[0] = ev_write_handle;
            signaler->GetContext().r[19] = kSignalResultAddr;
            REQUIRE(signaler->Run(kernel).IsSuccess());

            REQUIRE(h.WaitUntil([&] { return mem.Read32(kWaitDoneAddr) == 1; }));
            REQUIRE(mem.Read32(kWaitResultAddr) == ResultSuccess.GetInnerValue());
            REQUIRE(h.WaitUntil([&] {
                return waiter->GetState() == Kernel::ThreadState::Terminated &&
                       signaler->GetState() == Kernel::ThreadState::Terminated;
            }));
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const auto a = Kernel::KSchedulerTrace::Analyze(kernel.SchedulerTrace().Snapshot());
        result = {ms, a};
        });
        return result;
    };

    const auto [off_ms, off_analysis] = run_rounds(false);
    const auto [on_ms, on_analysis] = run_rounds(true);

    CHECK(off_analysis.total_records == 0);
    CHECK(on_analysis.completed_wakes >= static_cast<u64>(kRounds));
    CHECK(on_analysis.missed_wakes == 0);
    CHECK(on_analysis.runnable_unscheduled == 0);
    WARN("Progressing control overhead (" << kRounds << " cross-core wakes): trace_off=" << off_ms
         << " ms, trace_on=" << on_ms << " ms, delta=" << (on_ms - off_ms) << " ms; "
         << Kernel::KSchedulerTrace::FormatSummary(on_analysis));
}
