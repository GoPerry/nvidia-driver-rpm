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

#ifndef __UVM8_FORWARD_DECL_H__
#define __UVM8_FORWARD_DECL_H__

typedef struct uvm_gpu_struct uvm_gpu_t;
typedef struct uvm_rm_mem_struct uvm_rm_mem_t;
typedef struct uvm_mem_struct uvm_mem_t;
typedef struct uvm_host_hal_struct uvm_host_hal_t;
typedef struct uvm_ce_hal_struct uvm_ce_hal_t;
typedef struct uvm_arch_hal_struct uvm_arch_hal_t;
typedef struct uvm_fault_buffer_hal_struct uvm_fault_buffer_hal_t;
typedef struct uvm_gpu_semaphore_struct uvm_gpu_semaphore_t;
typedef struct uvm_gpu_tracking_semaphore_struct uvm_gpu_tracking_semaphore_t;
typedef struct uvm_gpu_semaphore_pool_struct uvm_gpu_semaphore_pool_t;
typedef struct uvm_gpu_semaphore_pool_page_struct uvm_gpu_semaphore_pool_page_t;
typedef struct uvm_gpu_peer_struct uvm_gpu_peer_t;
typedef struct uvm_mmu_mode_hal_struct uvm_mmu_mode_hal_t;

typedef struct uvm_channel_manager_struct uvm_channel_manager_t;
typedef struct uvm_channel_struct uvm_channel_t;
typedef struct uvm_push_struct uvm_push_t;
typedef struct uvm_push_info_struct uvm_push_info_t;
typedef struct uvm_pushbuffer_struct uvm_pushbuffer_t;
typedef struct uvm_gpfifo_entry_struct uvm_gpfifo_entry_t;

typedef struct uvm_va_range_struct uvm_va_range_t;
typedef struct uvm_va_block_struct uvm_va_block_t;
typedef struct uvm_va_space_struct uvm_va_space_t;

typedef struct uvm_gpu_va_space_struct uvm_gpu_va_space_t;

typedef struct uvm_thread_context_struct uvm_thread_context_t;

typedef struct uvm_perf_module_struct uvm_perf_module_t;

typedef struct uvm_page_table_range_vec_struct uvm_page_table_range_vec_t;

typedef struct uvm_gpu_next_data_struct uvm_gpu_next_data_t;
typedef struct uvm_fault_source_next_data_struct uvm_fault_source_next_data_t;
typedef struct uvm_fault_buffer_entry_next_data_struct uvm_fault_buffer_entry_next_data_t;

typedef struct uvm_fault_buffer_entry_struct uvm_fault_buffer_entry_t;

typedef struct uvm_pte_batch_struct uvm_pte_batch_t;
typedef struct uvm_tlb_batch_struct uvm_tlb_batch_t;

typedef struct uvm_fault_service_batch_context_struct uvm_fault_service_batch_context_t;
typedef struct uvm_fault_service_block_context_struct uvm_fault_service_block_context_t;

typedef struct uvm_replayable_fault_buffer_info_struct uvm_replayable_fault_buffer_info_t;

#endif //__UVM8_FORWARD_DECL_H__
