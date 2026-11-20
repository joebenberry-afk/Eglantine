// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/kernel/k_hardware_timer.h"
#include "core/hle/kernel/k_thread_queue.h"
#include "core/hle/kernel/kernel.h"

namespace Kernel {

void KThreadQueue::NotifyAvailable(KernelCore& kernel, KThread* waiting_thread, KSynchronizationObject* signaled_object, Result wait_result) {
    UNREACHABLE();
}

void KThreadQueue::EndWait(KernelCore& kernel, KThread* waiting_thread, Result wait_result) {
    if (kernel.IsSchedulerTraceEnabled()) [[unlikely]] {
        kernel.SchedulerTrace().Record({
            .virt_ns = kernel.CurrentTraceTimeNs(),
            .thread_key = KSchedulerTrace::ThreadKey(waiting_thread),
            .wait_id = waiting_thread->GetTraceWaitId(),
            .emitter_core = kernel.CurrentEmitterCoreId(),
            .subject_core = waiting_thread->GetActiveCore(),
            .priority = waiting_thread->GetPriority(),
            .affinity_mask =
                static_cast<u32>(waiting_thread->GetAffinityMask().GetAffinityMask()),
            .aux = wait_result.GetInnerValue(),
            .kind = KSchedulerTrace::EventKind::WaitOutcome,
            .sub_kind = 0,
        });
    }

    // Set the thread's wait result.
    waiting_thread->SetWaitResult(wait_result);

    // Set the thread as runnable.
    waiting_thread->SetState(kernel, ThreadState::Runnable);

    // Clear the thread's wait queue.
    waiting_thread->ClearWaitQueue();

    // Cancel the thread task.
    if (m_hardware_timer != nullptr) {
        m_hardware_timer->CancelTask(waiting_thread);
    }
}

void KThreadQueue::CancelWait(KernelCore& kernel, KThread* waiting_thread, Result wait_result, bool cancel_timer_task) {
    if (kernel.IsSchedulerTraceEnabled()) [[unlikely]] {
        kernel.SchedulerTrace().Record({
            .virt_ns = kernel.CurrentTraceTimeNs(),
            .thread_key = KSchedulerTrace::ThreadKey(waiting_thread),
            .wait_id = waiting_thread->GetTraceWaitId(),
            .emitter_core = kernel.CurrentEmitterCoreId(),
            .subject_core = waiting_thread->GetActiveCore(),
            .priority = waiting_thread->GetPriority(),
            .affinity_mask =
                static_cast<u32>(waiting_thread->GetAffinityMask().GetAffinityMask()),
            .aux = wait_result.GetInnerValue(),
            .kind = KSchedulerTrace::EventKind::WaitOutcome,
            .sub_kind = 1,
        });
    }

    // Set the thread's wait result.
    waiting_thread->SetWaitResult(wait_result);

    // Set the thread as runnable.
    waiting_thread->SetState(kernel, ThreadState::Runnable);

    // Clear the thread's wait queue.
    waiting_thread->ClearWaitQueue();

    // Cancel the thread task.
    if (cancel_timer_task && m_hardware_timer != nullptr) {
        m_hardware_timer->CancelTask(waiting_thread);
    }
}

void KThreadQueueWithoutEndWait::EndWait(KernelCore& kernel, KThread* waiting_thread, Result wait_result) {
    UNREACHABLE();
}

} // namespace Kernel
