// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/hle/kernel/k_scheduler_trace.h"

#include <algorithm>
#include <chrono>
#include <unordered_map>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/svc_results.h"

namespace Kernel {

namespace {

struct SpanRecord {
    u64 thread_key{};
    u64 wait_id{};
    u64 object_key{};
    u64 handoff_gen{};
    u64 deadline_virt_ns{};
    s32 subject_core{-1};
    s32 priority{};
    u32 affinity_mask{};
    u8 wait_kind{};
    u8 signal_outcome{};
    u32 wait_outcome_result{};
    bool from_preempt{false};

    bool wait_begin_seen{false};
    u64 wait_begin_virt_ns{};
    u64 wait_begin_host_ns{};

    bool signal_seen{false};
    u64 signal_virt_ns{};
    u64 signal_host_ns{};

    bool wait_outcome_seen{false};

    bool runnable_seen{false};
    u64 runnable_virt_ns{};
    u64 runnable_host_ns{};

    bool handoff_seen{false};
    u64 handoff_virt_ns{};
    u64 handoff_host_ns{};
    s32 handoff_core{-1};

    bool guest_enter_seen{false};
    u64 guest_enter_virt_ns{};
    u64 guest_enter_host_ns{};

    u64 select_opportunities{};
    u64 last_incumbent_key{};
    s32 last_incumbent_priority{};
};

struct CoreState {
    u64 incumbent_key{};
    s32 incumbent_priority{};
    bool incumbent_guest_entered{false};
    bool parked{false};
};

class AliasTable {
public:
    std::string Thread(u64 key) {
        if (key == 0) {
            return "none";
        }
        auto [it, inserted] = m_threads.try_emplace(key, m_threads.size() + 1);
        return fmt::format("t{}", it->second);
    }

    std::string Object(u64 key) {
        if (key == 0) {
            return "none";
        }
        auto [it, inserted] = m_objects.try_emplace(key, m_objects.size() + 1);
        return fmt::format("obj{}", it->second);
    }

    [[nodiscard]] u64 ThreadCount() const noexcept {
        return m_threads.size();
    }

    [[nodiscard]] u64 ObjectCount() const noexcept {
        return m_objects.size();
    }

private:
    std::unordered_map<u64, u64> m_threads;
    std::unordered_map<u64, u64> m_objects;
};

u64 NonNegativeDelta(u64 end_ns, u64 start_ns) noexcept {
    return end_ns >= start_ns ? (end_ns - start_ns) : 0;
}

bool IsEligibleSignal(u8 outcome) noexcept {
    const auto sig = static_cast<KSchedulerTrace::SignalOutcome>(outcome);
    return sig == KSchedulerTrace::SignalOutcome::WakeReady ||
           sig == KSchedulerTrace::SignalOutcome::MutexHandoff ||
           sig == KSchedulerTrace::SignalOutcome::TimerExpired ||
           sig == KSchedulerTrace::SignalOutcome::Cancelled;
}

KSchedulerTrace::LatencyStats ComputeLatencyStats(std::vector<u64>& samples) {
    KSchedulerTrace::LatencyStats stats{};
    if (samples.empty()) {
        return stats;
    }
    std::sort(samples.begin(), samples.end());
    stats.count = samples.size();
    stats.min_ns = samples.front();
    stats.max_ns = samples.back();
    stats.median_ns = samples[samples.size() / 2];
    const size_t p95_idx = ((samples.size() - 1) * 95) / 100;
    stats.p95_ns = samples[p95_idx];
    return stats;
}

struct HandoffKey {
    u64 thread_key{};
    u64 handoff_gen{};

    bool operator==(const HandoffKey&) const = default;
};

struct HandoffKeyHash {
    size_t operator()(const HandoffKey& k) const noexcept {
        return std::hash<u64>{}(k.thread_key) ^ (std::hash<u64>{}(k.handoff_gen) * 0x9e3779b97f4a7c15ULL);
    }
};

struct WaitKey {
    u64 thread_key{};
    u64 wait_id{};

    bool operator==(const WaitKey&) const = default;
};

struct WaitKeyHash {
    size_t operator()(const WaitKey& k) const noexcept {
        return std::hash<u64>{}(k.thread_key) ^ (std::hash<u64>{}(k.wait_id) * 0x9e3779b97f4a7c15ULL);
    }
};

} // namespace

KSchedulerTrace::KSchedulerTrace() = default;
KSchedulerTrace::~KSchedulerTrace() = default;

u64 KSchedulerTrace::HostNowNs() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

u64 KSchedulerTrace::ThreadKey(const KThread* thread) noexcept {
    if (thread == nullptr) {
        return 0;
    }
    const u64 tid = thread->GetThreadId();
    return tid != 0 ? tid : static_cast<u64>(reinterpret_cast<uintptr_t>(thread));
}

void KSchedulerTrace::EnsureAllocated(size_t capacity_per_producer) {
    const size_t cap = std::max<size_t>(capacity_per_producer, 16);
    if (m_capacity != cap || m_producers[0].ring.size() != cap) {
        m_capacity = cap;
        for (auto& prod : m_producers) {
            prod.ring.assign(cap, TransitionRecord{});
            prod.write_count.store(0, std::memory_order_relaxed);
        }
    }
}

void KSchedulerTrace::SetEnabled(bool enabled, size_t capacity_per_producer) {
    std::scoped_lock lk{m_config_mutex};
    if (enabled) {
        EnsureAllocated(capacity_per_producer);
        if (m_anchor_host_ns.load(std::memory_order_relaxed) == 0) {
            m_anchor_host_ns.store(HostNowNs(), std::memory_order_relaxed);
        }
    }
    m_enabled.store(enabled, std::memory_order_release);
}

void KSchedulerTrace::Reset(u64 initial_virt_ns) {
    std::scoped_lock lk{m_config_mutex};
    const bool was_enabled = m_enabled.exchange(false, std::memory_order_acq_rel);
    m_global_seq.store(0, std::memory_order_relaxed);
    m_anchor_host_ns.store(HostNowNs(), std::memory_order_relaxed);
    m_anchor_virt_ns.store(initial_virt_ns, std::memory_order_relaxed);
    m_imm_already_signaled.store(0, std::memory_order_relaxed);
    m_imm_zero_timeout.store(0, std::memory_order_relaxed);
    m_imm_cancelled.store(0, std::memory_order_relaxed);
    m_imm_terminating.store(0, std::memory_order_relaxed);
    for (auto& prod : m_producers) {
        std::scoped_lock plk{prod.non_core_mutex};
        prod.write_count.store(0, std::memory_order_relaxed);
        std::fill(prod.ring.begin(), prod.ring.end(), TransitionRecord{});
    }
    if (was_enabled) {
        EnsureAllocated(m_capacity);
        m_enabled.store(true, std::memory_order_release);
    }
}

void KSchedulerTrace::Record(const TransitionRecord& record) {
    if (!m_enabled.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }

    const size_t prod_idx =
        (record.emitter_core >= 0 && record.emitter_core < static_cast<s32>(NumCoreProducers))
            ? static_cast<size_t>(record.emitter_core)
            : NonCoreProducerIndex;
    auto& prod = m_producers[prod_idx];

    if (prod_idx == NonCoreProducerIndex) {
        std::scoped_lock lk{prod.non_core_mutex};
        const size_t cap = prod.ring.size();
        if (cap == 0) {
            return;
        }
        const u64 w = prod.write_count.load(std::memory_order_relaxed);
        TransitionRecord copy = record;
        copy.seq = m_global_seq.fetch_add(1, std::memory_order_relaxed) + 1;
        if (copy.host_ns == 0) {
            copy.host_ns = HostNowNs();
        }
        prod.ring[static_cast<size_t>(w % cap)] = copy;
        prod.write_count.store(w + 1, std::memory_order_release);
        return;
    }

    const size_t cap = prod.ring.size();
    if (cap == 0) {
        return;
    }
    const u64 w = prod.write_count.load(std::memory_order_relaxed);
    TransitionRecord copy = record;
    copy.seq = m_global_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    if (copy.host_ns == 0) {
        copy.host_ns = HostNowNs();
    }
    prod.ring[static_cast<size_t>(w % cap)] = copy;
    prod.write_count.store(w + 1, std::memory_order_release);
}

void KSchedulerTrace::RecordImmediate(WaitKind, u32 reason) {
    if (!m_enabled.load(std::memory_order_relaxed)) [[likely]] {
        return;
    }
    switch (reason) {
    case 1:
        m_imm_already_signaled.fetch_add(1, std::memory_order_relaxed);
        break;
    case 2:
        m_imm_zero_timeout.fetch_add(1, std::memory_order_relaxed);
        break;
    case 3:
        m_imm_cancelled.fetch_add(1, std::memory_order_relaxed);
        break;
    case 4:
        m_imm_terminating.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        break;
    }
}

KSchedulerTrace::SnapshotResult KSchedulerTrace::Snapshot() const {
    SnapshotResult out;
    out.anchor_host_ns = m_anchor_host_ns.load(std::memory_order_relaxed);
    out.anchor_virt_ns = m_anchor_virt_ns.load(std::memory_order_relaxed);
    out.immediate.already_signaled = m_imm_already_signaled.load(std::memory_order_relaxed);
    out.immediate.zero_timeout = m_imm_zero_timeout.load(std::memory_order_relaxed);
    out.immediate.cancelled = m_imm_cancelled.load(std::memory_order_relaxed);
    out.immediate.terminating = m_imm_terminating.load(std::memory_order_relaxed);

    for (size_t i = 0; i < NumProducers; ++i) {
        const auto& prod = m_producers[i];
        const size_t cap = prod.ring.size();
        out.buffer_bytes += cap * sizeof(TransitionRecord);
        if (cap == 0) {
            continue;
        }
        const u64 written = prod.write_count.load(std::memory_order_acquire);
        const u64 retained = std::min<u64>(written, cap);
        const u64 overwritten = written > cap ? (written - cap) : 0;
        out.producer_stats[i] = ProducerStats{
            .written = written,
            .retained = retained,
            .overwritten = overwritten,
        };
        out.total_written += written;
        out.total_overwritten += overwritten;

        const u64 start = written - retained;
        for (u64 idx = start; idx < written; ++idx) {
            const auto& rec = prod.ring[static_cast<size_t>(idx % cap)];
            if (rec.seq != 0) {
                out.records.push_back(rec);
            }
        }
    }

    std::sort(out.records.begin(), out.records.end(),
              [](const TransitionRecord& a, const TransitionRecord& b) { return a.seq < b.seq; });
    return out;
}

KSchedulerTrace::TraceAnalysis KSchedulerTrace::Analyze(const SnapshotResult& snapshot) {
    TraceAnalysis analysis{};
    analysis.total_records = snapshot.total_written;
    analysis.retained_records = snapshot.records.size();
    analysis.overwritten_records = snapshot.total_overwritten;
    analysis.buffer_bytes = snapshot.buffer_bytes;
    analysis.immediate = snapshot.immediate;

    AliasTable aliases;
    std::vector<SpanRecord> spans;
    std::unordered_map<WaitKey, size_t, WaitKeyHash> wait_index;
    std::unordered_map<u64, size_t> open_runnable_by_thread;
    std::unordered_map<HandoffKey, size_t, HandoffKeyHash> handoff_index;
    std::array<CoreState, NumCoreProducers> cores{};

    const auto GetOrCreateWaitSpan = [&](u64 thread_key, u64 wait_id) -> size_t {
        const WaitKey key{thread_key, wait_id};
        auto it = wait_index.find(key);
        if (it != wait_index.end()) {
            return it->second;
        }
        const size_t idx = spans.size();
        spans.push_back(SpanRecord{.thread_key = thread_key, .wait_id = wait_id});
        wait_index.emplace(key, idx);
        return idx;
    };

    u64 max_virt_ns = 0;
    u64 max_host_ns = 0;

    for (const auto& rec : snapshot.records) {
        max_virt_ns = std::max(max_virt_ns, rec.virt_ns);
        max_host_ns = std::max(max_host_ns, rec.host_ns);

        if (rec.thread_key != 0 && (rec.flags & 1U) == 0) {
            (void)aliases.Thread(rec.thread_key);
        }
        if (rec.secondary_thread_key != 0 && (rec.flags & 2U) == 0) {
            (void)aliases.Thread(rec.secondary_thread_key);
        }
        if (rec.object_key != 0) {
            (void)aliases.Object(rec.object_key);
        }

        switch (rec.kind) {
        case EventKind::WaitBegin: {
            const size_t idx = GetOrCreateWaitSpan(rec.thread_key, rec.wait_id);
            auto& s = spans[idx];
            s.wait_begin_seen = true;
            s.wait_begin_virt_ns = rec.virt_ns;
            s.wait_begin_host_ns = rec.host_ns;
            s.wait_kind = rec.sub_kind;
            s.object_key = rec.object_key;
            s.deadline_virt_ns = rec.deadline_virt_ns;
            s.subject_core = rec.subject_core;
            s.priority = rec.priority;
            s.affinity_mask = rec.affinity_mask;
            break;
        }
        case EventKind::Signal: {
            const size_t idx = GetOrCreateWaitSpan(rec.thread_key, rec.wait_id);
            auto& s = spans[idx];
            if (IsEligibleSignal(rec.sub_kind)) {
                if (!s.signal_seen) {
                    s.signal_seen = true;
                    s.signal_virt_ns = rec.virt_ns;
                    s.signal_host_ns = rec.host_ns;
                    s.signal_outcome = rec.sub_kind;
                }
            } else if (!s.signal_seen) {
                s.signal_outcome = rec.sub_kind;
            }
            if (s.object_key == 0 && rec.object_key != 0) {
                s.object_key = rec.object_key;
            }
            break;
        }
        case EventKind::WaitOutcome: {
            const size_t idx = GetOrCreateWaitSpan(rec.thread_key, rec.wait_id);
            auto& s = spans[idx];
            s.wait_outcome_seen = true;
            s.wait_outcome_result = rec.aux;
            break;
        }
        case EventKind::Runnable: {
            const size_t idx = GetOrCreateWaitSpan(rec.thread_key, rec.wait_id);
            auto& s = spans[idx];
            if (!s.runnable_seen) {
                s.runnable_seen = true;
                s.runnable_virt_ns = rec.virt_ns;
                s.runnable_host_ns = rec.host_ns;
            }
            s.subject_core = rec.subject_core;
            s.priority = rec.priority;
            s.affinity_mask = rec.affinity_mask;
            open_runnable_by_thread[rec.thread_key] = idx;
            break;
        }
        case EventKind::SwitchOutRunnable: {
            const size_t idx = spans.size();
            spans.push_back(SpanRecord{
                .thread_key = rec.thread_key,
                .wait_id = rec.wait_id,
                .subject_core = rec.subject_core,
                .priority = rec.priority,
                .affinity_mask = rec.affinity_mask,
                .from_preempt = true,
                .runnable_seen = true,
                .runnable_virt_ns = rec.virt_ns,
                .runnable_host_ns = rec.host_ns,
            });
            open_runnable_by_thread[rec.thread_key] = idx;
            break;
        }
        case EventKind::NotRunnable: {
            open_runnable_by_thread.erase(rec.thread_key);
            break;
        }
        case EventKind::Migrate: {
            if (const auto it = open_runnable_by_thread.find(rec.thread_key);
                it != open_runnable_by_thread.end()) {
                auto& s = spans[it->second];
                s.subject_core = rec.subject_core;
                s.priority = rec.priority;
                s.affinity_mask = rec.affinity_mask;
            }
            break;
        }
        case EventKind::Select: {
            const s32 core_id = rec.emitter_core >= 0 ? rec.emitter_core : rec.subject_core;
            if (core_id >= 0 && core_id < static_cast<s32>(NumCoreProducers)) {
                const auto& c = cores[static_cast<size_t>(core_id)];
                const u32 core_bit = 1U << static_cast<u32>(core_id);
                for (const auto& [tkey, sidx] : open_runnable_by_thread) {
                    auto& s = spans[sidx];
                    if (s.runnable_seen && !s.handoff_seen &&
                        (s.subject_core == core_id || (s.affinity_mask & core_bit) != 0)) {
                        ++s.select_opportunities;
                        s.last_incumbent_key = c.incumbent_key;
                        s.last_incumbent_priority = c.incumbent_priority;
                    }
                }
            }
            break;
        }
        case EventKind::Handoff: {
            const bool is_idle = (rec.flags & 1U) != 0;
            const s32 core_id = rec.emitter_core >= 0 ? rec.emitter_core : rec.subject_core;
            if (core_id >= 0 && core_id < static_cast<s32>(NumCoreProducers)) {
                auto& c = cores[static_cast<size_t>(core_id)];
                c.incumbent_key = is_idle ? 0 : rec.thread_key;
                c.incumbent_priority = rec.priority;
                c.incumbent_guest_entered = false;
                if (!is_idle) {
                    c.parked = false;
                }
            }
            if (!is_idle && rec.thread_key != 0) {
                size_t idx{};
                if (const auto it = open_runnable_by_thread.find(rec.thread_key);
                    it != open_runnable_by_thread.end()) {
                    idx = it->second;
                    open_runnable_by_thread.erase(it);
                } else {
                    idx = GetOrCreateWaitSpan(rec.thread_key, rec.wait_id);
                }
                auto& s = spans[idx];
                s.handoff_seen = true;
                s.handoff_virt_ns = rec.virt_ns;
                s.handoff_host_ns = rec.host_ns;
                s.handoff_core = core_id;
                s.handoff_gen = rec.handoff_gen;
                if (rec.handoff_gen != 0) {
                    handoff_index[HandoffKey{rec.thread_key, rec.handoff_gen}] = idx;
                }
            }
            break;
        }
        case EventKind::GuestEnter: {
            const s32 core_id = rec.emitter_core >= 0 ? rec.emitter_core : rec.subject_core;
            if (core_id >= 0 && core_id < static_cast<s32>(NumCoreProducers)) {
                auto& c = cores[static_cast<size_t>(core_id)];
                c.incumbent_key = rec.thread_key;
                c.incumbent_priority = rec.priority;
                c.incumbent_guest_entered = true;
                c.parked = false;
            }
            if (rec.thread_key != 0 && rec.handoff_gen != 0) {
                if (const auto it = handoff_index.find(HandoffKey{rec.thread_key, rec.handoff_gen});
                    it != handoff_index.end()) {
                    auto& s = spans[it->second];
                    if (!s.guest_enter_seen) {
                        s.guest_enter_seen = true;
                        s.guest_enter_virt_ns = rec.virt_ns;
                        s.guest_enter_host_ns = rec.host_ns;
                    }
                }
            }
            break;
        }
        case EventKind::CorePark: {
            const s32 core_id = rec.emitter_core >= 0 ? rec.emitter_core : rec.subject_core;
            if (core_id >= 0 && core_id < static_cast<s32>(NumCoreProducers)) {
                auto& c = cores[static_cast<size_t>(core_id)];
                c.parked = true;
                c.incumbent_guest_entered = false;
                c.incumbent_key = 0;
            }
            break;
        }
        case EventKind::CoreWake: {
            const s32 core_id = rec.emitter_core >= 0 ? rec.emitter_core : rec.subject_core;
            if (core_id >= 0 && core_id < static_cast<s32>(NumCoreProducers)) {
                cores[static_cast<size_t>(core_id)].parked = false;
            }
            break;
        }
        case EventKind::WaitImmediate:
            break;
        }
    }

    analysis.unique_threads = aliases.ThreadCount();
    analysis.unique_objects = aliases.ObjectCount();

    std::vector<u64> sig_to_run_virt;
    std::vector<u64> sig_to_run_host;
    std::vector<u64> run_to_hand_virt;
    std::vector<u64> run_to_hand_host;
    std::vector<u64> hand_to_guest_virt;
    std::vector<u64> hand_to_guest_host;
    std::vector<u64> run_to_running_virt;
    std::vector<u64> run_to_running_host;

    for (const auto& s : spans) {
        if (s.runnable_seen && s.handoff_seen && s.guest_enter_seen) {
            if (s.from_preempt) {
                ++analysis.completed_preempt_resumes;
            } else if (s.signal_seen &&
                       static_cast<SignalOutcome>(s.signal_outcome) == SignalOutcome::TimerExpired &&
                       s.wait_outcome_seen &&
                       s.wait_outcome_result == ResultTimedOut.GetInnerValue()) {
                ++analysis.completed_timed_out;
            } else {
                ++analysis.completed_wakes;
            }

            if (s.signal_seen && IsEligibleSignal(s.signal_outcome)) {
                sig_to_run_virt.push_back(NonNegativeDelta(s.runnable_virt_ns, s.signal_virt_ns));
                sig_to_run_host.push_back(NonNegativeDelta(s.runnable_host_ns, s.signal_host_ns));
            }
            run_to_hand_virt.push_back(NonNegativeDelta(s.handoff_virt_ns, s.runnable_virt_ns));
            run_to_hand_host.push_back(NonNegativeDelta(s.handoff_host_ns, s.runnable_host_ns));
            hand_to_guest_virt.push_back(NonNegativeDelta(s.guest_enter_virt_ns, s.handoff_virt_ns));
            hand_to_guest_host.push_back(NonNegativeDelta(s.guest_enter_host_ns, s.handoff_host_ns));
            run_to_running_virt.push_back(NonNegativeDelta(s.guest_enter_virt_ns, s.runnable_virt_ns));
            run_to_running_host.push_back(NonNegativeDelta(s.guest_enter_host_ns, s.runnable_host_ns));
        } else if (s.handoff_seen && !s.guest_enter_seen) {
            ++analysis.handoff_without_guest_enter;
            analysis.findings.push_back(DiagnosticFinding{
                .kind = FindingKind::HandoffWithoutGuestEnter,
                .thread_alias = aliases.Thread(s.thread_key),
                .object_alias = aliases.Object(s.object_key),
                .incumbent_alias = "none",
                .subject_core = s.handoff_core,
                .priority = s.priority,
                .affinity_mask = s.affinity_mask,
                .wait_id = s.wait_id,
                .age_virt_ns = NonNegativeDelta(max_virt_ns, s.handoff_virt_ns),
                .age_host_ns = NonNegativeDelta(max_host_ns, s.handoff_host_ns),
            });
        } else if (s.runnable_seen && !s.handoff_seen) {
            ++analysis.runnable_unscheduled;

            bool spin_suspect = false;
            u64 incumbent_key = s.last_incumbent_key;
            s32 incumbent_prio = s.last_incumbent_priority;
            if (s.subject_core >= 0 && s.subject_core < static_cast<s32>(NumCoreProducers)) {
                const auto& c = cores[static_cast<size_t>(s.subject_core)];
                if (!c.parked && c.incumbent_guest_entered && c.incumbent_key != 0 &&
                    c.incumbent_key != s.thread_key) {
                    spin_suspect = true;
                    incumbent_key = c.incumbent_key;
                    incumbent_prio = c.incumbent_priority;
                }
            }
            if (spin_suspect) {
                ++analysis.guest_spin_suspects;
            }
            analysis.findings.push_back(DiagnosticFinding{
                .kind = FindingKind::RunnableUnscheduled,
                .thread_alias = aliases.Thread(s.thread_key),
                .object_alias = aliases.Object(s.object_key),
                .incumbent_alias = aliases.Thread(incumbent_key),
                .subject_core = s.subject_core,
                .priority = s.priority,
                .incumbent_priority = incumbent_prio,
                .affinity_mask = s.affinity_mask,
                .wait_id = s.wait_id,
                .select_opportunities = s.select_opportunities,
                .age_virt_ns = NonNegativeDelta(max_virt_ns, s.runnable_virt_ns),
                .age_host_ns = NonNegativeDelta(max_host_ns, s.runnable_host_ns),
                .guest_spin_suspect = spin_suspect,
            });
        } else if (s.wait_begin_seen && !s.runnable_seen) {
            if (s.signal_seen && IsEligibleSignal(s.signal_outcome) && !s.wait_outcome_seen) {
                ++analysis.missed_wakes;
                analysis.findings.push_back(DiagnosticFinding{
                    .kind = FindingKind::MissedWake,
                    .thread_alias = aliases.Thread(s.thread_key),
                    .object_alias = aliases.Object(s.object_key),
                    .incumbent_alias = "none",
                    .subject_core = s.subject_core,
                    .priority = s.priority,
                    .affinity_mask = s.affinity_mask,
                    .wait_id = s.wait_id,
                    .age_virt_ns = NonNegativeDelta(max_virt_ns, s.signal_virt_ns),
                    .age_host_ns = NonNegativeDelta(max_host_ns, s.signal_host_ns),
                });
            } else {
                ++analysis.ordinary_waits;
                analysis.findings.push_back(DiagnosticFinding{
                    .kind = FindingKind::OrdinaryWait,
                    .thread_alias = aliases.Thread(s.thread_key),
                    .object_alias = aliases.Object(s.object_key),
                    .incumbent_alias = "none",
                    .subject_core = s.subject_core,
                    .priority = s.priority,
                    .affinity_mask = s.affinity_mask,
                    .wait_id = s.wait_id,
                    .age_virt_ns = NonNegativeDelta(max_virt_ns, s.wait_begin_virt_ns),
                    .age_host_ns = NonNegativeDelta(max_host_ns, s.wait_begin_host_ns),
                });
            }
        }
    }

    analysis.signal_to_runnable_virt = ComputeLatencyStats(sig_to_run_virt);
    analysis.signal_to_runnable_host = ComputeLatencyStats(sig_to_run_host);
    analysis.runnable_to_handoff_virt = ComputeLatencyStats(run_to_hand_virt);
    analysis.runnable_to_handoff_host = ComputeLatencyStats(run_to_hand_host);
    analysis.handoff_to_guest_virt = ComputeLatencyStats(hand_to_guest_virt);
    analysis.handoff_to_guest_host = ComputeLatencyStats(hand_to_guest_host);
    analysis.runnable_to_running_virt = ComputeLatencyStats(run_to_running_virt);
    analysis.runnable_to_running_host = ComputeLatencyStats(run_to_running_host);

    return analysis;
}

std::string KSchedulerTrace::FormatSummary(const TraceAnalysis& a) {
    return fmt::format(
        "sched_trace summary: records_total={} retained={} overwritten={} buffer_bytes={} "
        "threads={} objects={} completed_wakes={} timed_out={} preempt_resumes={} "
        "ordinary_waits={} missed_wakes={} runnable_unscheduled={} handoff_no_guest={} "
        "guest_spin_suspects={} imm_signaled={} imm_zero={} imm_cancelled={} "
        "sig_to_run_virt_ns(n={},p50={},p95={},max={}) "
        "run_to_hand_virt_ns(n={},p50={},p95={},max={}) "
        "hand_to_guest_virt_ns(n={},p50={},p95={},max={}) "
        "run_to_running_virt_ns(n={},p50={},p95={},max={}) "
        "run_to_running_host_ns(n={},p50={},p95={},max={})",
        a.total_records, a.retained_records, a.overwritten_records, a.buffer_bytes,
        a.unique_threads, a.unique_objects, a.completed_wakes, a.completed_timed_out,
        a.completed_preempt_resumes, a.ordinary_waits, a.missed_wakes, a.runnable_unscheduled,
        a.handoff_without_guest_enter, a.guest_spin_suspects, a.immediate.already_signaled,
        a.immediate.zero_timeout, a.immediate.cancelled, a.signal_to_runnable_virt.count,
        a.signal_to_runnable_virt.median_ns, a.signal_to_runnable_virt.p95_ns,
        a.signal_to_runnable_virt.max_ns, a.runnable_to_handoff_virt.count,
        a.runnable_to_handoff_virt.median_ns, a.runnable_to_handoff_virt.p95_ns,
        a.runnable_to_handoff_virt.max_ns, a.handoff_to_guest_virt.count,
        a.handoff_to_guest_virt.median_ns, a.handoff_to_guest_virt.p95_ns,
        a.handoff_to_guest_virt.max_ns, a.runnable_to_running_virt.count,
        a.runnable_to_running_virt.median_ns, a.runnable_to_running_virt.p95_ns,
        a.runnable_to_running_virt.max_ns, a.runnable_to_running_host.count,
        a.runnable_to_running_host.median_ns, a.runnable_to_running_host.p95_ns,
        a.runnable_to_running_host.max_ns);
}

void KSchedulerTrace::LogSanitizedSummary() const {
    if (!IsEnabled() && m_global_seq.load(std::memory_order_relaxed) == 0) {
        return;
    }
    const auto snapshot = Snapshot();
    const auto analysis = Analyze(snapshot);
    LOG_INFO(Kernel, "{}", FormatSummary(analysis));

    constexpr size_t MaxLoggedFindings = 32;
    size_t logged = 0;
    for (const auto& f : analysis.findings) {
        if (f.kind == FindingKind::OrdinaryWait) {
            continue;
        }
        if (logged >= MaxLoggedFindings) {
            break;
        }
        const char* kind_str = "unknown";
        switch (f.kind) {
        case FindingKind::MissedWake:
            kind_str = "missed_wake";
            break;
        case FindingKind::RunnableUnscheduled:
            kind_str = "runnable_unscheduled";
            break;
        case FindingKind::HandoffWithoutGuestEnter:
            kind_str = "handoff_no_guest";
            break;
        case FindingKind::GuestSpinSuspect:
            kind_str = "guest_spin";
            break;
        case FindingKind::OrdinaryWait:
            kind_str = "ordinary_wait";
            break;
        }
        LOG_INFO(Kernel,
                 "sched_trace finding kind={} thread={} wait_id={} obj={} core={} prio={} "
                 "mask={:#x} select_ops={} incumbent={} inc_prio={} spin_suspect={} "
                 "age_virt_ns={} age_host_ns={}",
                 kind_str, f.thread_alias, f.wait_id, f.object_alias, f.subject_core, f.priority,
                 f.affinity_mask, f.select_opportunities, f.incumbent_alias, f.incumbent_priority,
                 f.guest_spin_suspect ? 1 : 0, f.age_virt_ns, f.age_host_ns);
        ++logged;
    }
}

} // namespace Kernel
