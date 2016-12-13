/*******************************************************************************
    Copyright (c) 2015 NVIDIA Corporation

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

        The above copyright notice and this permission notice shall be
        included in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

*******************************************************************************/

#ifndef __UVM8_GPU_PAGE_FAULT_H__
#define __UVM8_GPU_PAGE_FAULT_H__

#include "nvtypes.h"
#include "uvmtypes.h"
#include "uvm8_hal_types.h"
#include "uvm8_tracker.h"

typedef enum
{
    // Issue a fault replay after all faults for a block within a batch have been serviced
    UVM_PERF_FAULT_REPLAY_POLICY_BLOCK = 0,

    // Issue a fault replay after each fault batch has been serviced
    UVM_PERF_FAULT_REPLAY_POLICY_BATCH,

    // Like UVM_PERF_FAULT_REPLAY_POLICY_BATCH but only one batch of faults is serviced. The fault buffer is flushed
    // before issuing the replay. The potential benefit is that we can resume execution of some SMs earlier, if SMs
    // are faulting on different sets of pages.
    UVM_PERF_FAULT_REPLAY_POLICY_BATCH_FLUSH,

    // Issue a fault replay after all faults in the buffer have been serviced
    UVM_PERF_FAULT_REPLAY_POLICY_ONCE,

    // TODO: Bug 1768226: Implement uTLB-aware fault replay policy

    UVM_PERF_FAULT_REPLAY_POLICY_MAX,
} uvm_perf_fault_replay_policy_t;

const char *uvm_perf_fault_replay_policy_string(uvm_perf_fault_replay_policy_t fault_replay);

NV_STATUS uvm_gpu_fault_buffer_init(uvm_gpu_t *gpu);
void uvm_gpu_fault_buffer_deinit(uvm_gpu_t *gpu);

NV_STATUS uvm8_isr_top_half(NvProcessorUuid *gpu_uuid);
bool uvm_replayable_faults_pending(uvm_gpu_t *gpu);

// Clear valid bit for all remaining unserviced faults in the buffer, set GET to
// PUT, and push a fault replay of type UVM_FAULT_REPLAY_TYPE_START. It does not
// wait for the replay to complete before returning. The pushed replay is added
// to the GPU's replay_tracker.
//
// LOCKING: Takes gpu->isr_lock
NV_STATUS uvm_gpu_fault_buffer_flush(uvm_gpu_t *gpu);

// Increments the reference count tracking whether replayable page fault
// interrupts should be enabled. The caller is guaranteed that replayable page
// faults are disabled upon return. Interrupts might already be disabled prior
// to making this call. Each call is ref-counted, so this must be paired with a
// call to uvm_gpu_enable_replayable_faults().
//
// gpu->page_fault_interrupts_lock must be held to call this function.
void uvm_gpu_disable_replayable_faults(uvm_gpu_t *gpu);

// Decrements the reference count tracking whether replayable page fault
// interrupts should be enabled. Only once the count reaches 0 are the HW
// interrupts actually enabled, so this call does not guarantee that the
// interrupts have been re-enabled upon return.
//
// uvm_gpu_disable_replayable_faults() must have been called prior to calling
// this function.
//
// gpu->page_fault_interrupts_lock must be held to call this function.
void uvm_gpu_enable_replayable_faults(uvm_gpu_t *gpu);

// Take the gpu->isr_lock from a non-top/bottom half thread. This will also
// disable replayable page fault interrupts (if supported by the GPU) because
// the top half attempts to take this lock, and we would cause an interrupt
// storm if we didn't disable them first.
//
// The GPU must have been previously retained.
void uvm_gpu_isr_lock(uvm_gpu_t *gpu);

// Unlock the gpu->isr_lock, possibly re-enabling replayable page fault
// interrupts. Unlike uvm_gpu_isr_lock(), which should only be called from non-
// top/bottom half threads, this can be called by any thread.
void uvm_gpu_isr_unlock(uvm_gpu_t *gpu);

// For use by the nv_kthread_q that is servicing the bottom half, only.
void uvm8_isr_bottom_half(void *args);

// Enable/disable HW support for prefetch-initiated faults
void uvm_gpu_enable_prefetch_faults(uvm_gpu_t *gpu);
void uvm_gpu_disable_prefetch_faults(uvm_gpu_t *gpu);

#endif // __UVM8_GPU_PAGE_FAULT_H__