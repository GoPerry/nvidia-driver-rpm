/*******************************************************************************
    Copyright (c) 2016 NVIDIA Corporation

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

#ifndef __UVM_TOOLS_H__
#define __UVM_TOOLS_H__

#include "uvmtypes.h"
#include "uvm8_processors.h"
#include "uvm8_forward_decl.h"
#include "uvm8_test_ioctl.h"

NV_STATUS uvm8_test_inject_tools_event(UVM_TEST_INJECT_TOOLS_EVENT_PARAMS *params, struct file *filp);
NV_STATUS uvm8_test_increment_tools_counter(UVM_TEST_INCREMENT_TOOLS_COUNTER_PARAMS *params, struct file *filp);

NV_STATUS uvm_api_tools_read_process_memory(UVM_TOOLS_READ_PROCESS_MEMORY_PARAMS *params, struct file *filp);
NV_STATUS uvm_api_tools_write_process_memory(UVM_TOOLS_WRITE_PROCESS_MEMORY_PARAMS *params, struct file *filp);
NV_STATUS uvm_api_tools_get_processor_uuid_table(UVM_TOOLS_GET_PROCESSOR_UUID_TABLE_PARAMS *params, struct file *filp);
NV_STATUS uvm_api_tools_flush_events(UVM_TOOLS_FLUSH_EVENTS_PARAMS *params, struct file *filp);

// TODO: Bug 1760246: Temporary workaround to start recording replay events. The
//       final implementation should provide a VA space broadcast mechanism.
void uvm_tools_record_replay(uvm_gpu_id_t gpu_id, uvm_va_space_t *va_space, NvU32 batch_id);

static UvmEventFatalReason uvm_tools_status_to_fatal_fault_reason(NV_STATUS status)
{
    switch (status) {
        case NV_OK:
            return UvmEventFatalReasonInvalid;
        case NV_ERR_NO_MEMORY:
            return UvmEventFatalReasonOutOfMemory;
        case NV_ERR_INVALID_ADDRESS:
            return UvmEventFatalReasonInvalidAddress;
        case NV_ERR_INVALID_ACCESS_TYPE:
            return UvmEventFatalReasonInvalidPermissions;
        case NV_ERR_INVALID_OPERATION:
            return UvmEventFatalReasonInvalidOperation;
        default:
            return UvmEventFatalReasonInternalError;
    }
}

void uvm_tools_record_cpu_fatal_fault(uvm_va_space_t *va_space, NvU64 address, bool is_write,
                                      UvmEventFatalReason reason);

void uvm_tools_record_gpu_fatal_fault(uvm_gpu_id_t gpu_id,
                                      uvm_va_space_t *va_space,
                                      uvm_fault_buffer_entry_t *fault_entry,
                                      UvmEventFatalReason reason);

void uvm_tools_record_thrashing(uvm_va_block_t *va_block, NvU64 address, size_t region_size,
                                const uvm_processor_mask_t *processors);

void uvm_tools_record_throttling_start(uvm_va_block_t *va_block, NvU64 address, uvm_processor_id_t processor);

void uvm_tools_record_throttling_end(uvm_va_block_t *va_block, NvU64 address, uvm_processor_id_t processor);

void uvm_tools_record_map_remote(uvm_va_block_t *va_block, uvm_processor_id_t processor, uvm_processor_id_t residency,
                                 NvU64 address, size_t region_size, UvmEventMapRemoteCause cause);

void uvm_tools_broadcast_replay(uvm_gpu_id_t gpu_id, NvU32 batch_id);
// Invokes the pushbuffer reclamation for the VA space
void uvm_tools_schedule_completed_events(uvm_va_space_t *va_space);

// schedules completed events and then waits from the to be dispatched
void uvm_tools_flush_events(uvm_va_space_t *va_space);

#endif // __UVM_TOOLS_H__
