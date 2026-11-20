// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "core/hardware_properties.h"

namespace Kernel {

class KThread;

/// Bounded, lock-free-per-core transition trace for correlating wait, readiness/signal,
/// runnable queue insertion, scheduler selection, committed handoff, and guest entry.
class KSchedulerTrace final {
public:
    static constexpr size_t DefaultProducerCapacity = 4096;
    static constexpr size_t NumCoreProducers = Core::Hardware::NUM_CPU_CORES;
    static constexpr size_t NonCoreProducerIndex = NumCoreProducers;
    static constexpr size_t NumProducers = NumCoreProducers + 1;

    enum class EventKind : u8 {
        WaitBegin = 0,
        WaitImmediate = 1,
        Signal = 2,
        WaitOutcome = 3,
        Runnable = 4,
        NotRunnable = 5,
        Migrate = 6,
        Select = 7,
        Handoff = 8,
        SwitchOutRunnable = 9,
        GuestEnter = 10,
        CorePark = 11,
        CoreWake = 12,
    };

    enum class WaitKind : u8 {
        None = 0,
        Synchronization = 1,
        Sleep = 2,
        ConditionVar = 3,
        ArbiterMutex = 4,
        Arbiter = 5,
        Pin = 6,
    };

    enum class SignalOutcome : u8 {
        WakeReady = 0,
        MovedToMutex = 1,
        MutexHandoff = 2,
        TimerExpired = 3,
        Cancelled = 4,
        InvalidOwner = 5,
    };

    enum class FindingKind : u8 {
        OrdinaryWait = 0,
        MissedWake = 1,
        RunnableUnscheduled = 2,
        HandoffWithoutGuestEnter = 3,
        GuestSpinSuspect = 4,
    };

    struct TransitionRecord {
        u64 seq{};
        u64 virt_ns{};
        u64 host_ns{};
        u64 thread_key{};
        u64 wait_id{};
        u64 object_key{};
        u64 handoff_gen{};
        u64 deadline_virt_ns{};
        u64 secondary_thread_key{};
        s32 emitter_core{-1};
        s32 subject_core{-1};
        s32 priority{};
        s32 secondary_priority{};
        u32 affinity_mask{};
        u32 aux{};
        EventKind kind{};
        u8 sub_kind{};
        u8 flags{};
        u8 reserved{};
    };

    struct ImmediateCounters {
        u64 already_signaled{};
        u64 zero_timeout{};
        u64 cancelled{};
        u64 terminating{};
    };

    struct ProducerStats {
        u64 written{};
        u64 retained{};
        u64 overwritten{};
    };

    struct SnapshotResult {
        std::vector<TransitionRecord> records;
        std::array<ProducerStats, NumProducers> producer_stats{};
        ImmediateCounters immediate{};
        u64 total_written{};
        u64 total_overwritten{};
        u64 buffer_bytes{};
        u64 anchor_host_ns{};
        u64 anchor_virt_ns{};
    };

    struct LatencyStats {
        u64 count{};
        u64 min_ns{};
        u64 median_ns{};
        u64 p95_ns{};
        u64 max_ns{};
    };

    struct DiagnosticFinding {
        FindingKind kind{};
        std::string thread_alias;
        std::string object_alias;
        std::string incumbent_alias;
        s32 subject_core{-1};
        s32 priority{};
        s32 incumbent_priority{};
        u32 affinity_mask{};
        u64 wait_id{};
        u64 select_opportunities{};
        u64 age_virt_ns{};
        u64 age_host_ns{};
        bool guest_spin_suspect{false};
    };

    struct TraceAnalysis {
        u64 total_records{};
        u64 retained_records{};
        u64 overwritten_records{};
        u64 buffer_bytes{};
        u64 unique_threads{};
        u64 unique_objects{};
        u64 completed_wakes{};
        u64 completed_timed_out{};
        u64 completed_preempt_resumes{};
        u64 ordinary_waits{};
        u64 missed_wakes{};
        u64 runnable_unscheduled{};
        u64 handoff_without_guest_enter{};
        u64 guest_spin_suspects{};
        ImmediateCounters immediate{};
        LatencyStats signal_to_runnable_virt;
        LatencyStats signal_to_runnable_host;
        LatencyStats runnable_to_handoff_virt;
        LatencyStats runnable_to_handoff_host;
        LatencyStats handoff_to_guest_virt;
        LatencyStats handoff_to_guest_host;
        LatencyStats runnable_to_running_virt;
        LatencyStats runnable_to_running_host;
        std::vector<DiagnosticFinding> findings;
    };

    KSchedulerTrace();
    ~KSchedulerTrace();

    KSchedulerTrace(const KSchedulerTrace&) = delete;
    KSchedulerTrace& operator=(const KSchedulerTrace&) = delete;

    void SetEnabled(bool enabled, size_t capacity_per_producer = DefaultProducerCapacity);
    [[nodiscard]] bool IsEnabled() const noexcept {
        return m_enabled.load(std::memory_order_relaxed);
    }

    void Reset(u64 initial_virt_ns = 0);

    void Record(const TransitionRecord& record);

    void RecordImmediate(WaitKind wait_kind, u32 reason);

    [[nodiscard]] SnapshotResult Snapshot() const;
    [[nodiscard]] static TraceAnalysis Analyze(const SnapshotResult& snapshot);
    [[nodiscard]] static std::string FormatSummary(const TraceAnalysis& analysis);

    void LogSanitizedSummary() const;

    [[nodiscard]] static u64 ThreadKey(const KThread* thread) noexcept;

private:
    struct ProducerBuffer {
        std::vector<TransitionRecord> ring;
        std::atomic<u64> write_count{0};
        std::mutex non_core_mutex;
    };

    void EnsureAllocated(size_t capacity_per_producer);
    [[nodiscard]] static u64 HostNowNs() noexcept;

    std::atomic<bool> m_enabled{false};
    std::atomic<u64> m_global_seq{0};
    std::atomic<u64> m_anchor_host_ns{0};
    std::atomic<u64> m_anchor_virt_ns{0};
    size_t m_capacity{DefaultProducerCapacity};
    std::mutex m_config_mutex;
    std::array<ProducerBuffer, NumProducers> m_producers;

    std::atomic<u64> m_imm_already_signaled{0};
    std::atomic<u64> m_imm_zero_timeout{0};
    std::atomic<u64> m_imm_cancelled{0};
    std::atomic<u64> m_imm_terminating{0};
};

} // namespace Kernel
