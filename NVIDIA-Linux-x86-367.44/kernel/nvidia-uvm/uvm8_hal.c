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

#include "uvm8_hal.h"
#include "uvm8_kvmalloc.h"
#include "uvm8_next_decl.h"

#include "cla06f.h"
#include "cla06fsubch.h"
#include "cla16f.h"
#include "cla0b5.h"
#include "clb069.h"
#include "clb069sw.h"
#include "clb06f.h"
#include "clb0b5.h"
#include "clc06f.h"
#include "clc0b5.h"
#include "clc1b5.h"
#include "ctrl2080mc.h"

#define ce_class_count_base() ARRAY_SIZE(ce_table_base)
#define host_class_count_base() ARRAY_SIZE(host_table_base)
#define arch_class_count_base() ARRAY_SIZE(arch_table_base)
#define fault_buffer_class_count_base() ARRAY_SIZE(fault_buffer_table_base)

#define ce_class_count() (ce_class_count_base() + ce_class_count_next())
#define host_class_count() (host_class_count_base() + host_class_count_next())
#define arch_class_count() (arch_class_count_base() + arch_class_count_next())
#define fault_buffer_class_count() (fault_buffer_class_count_base() + fault_buffer_class_count_next())

#define CE_OP_COUNT (sizeof(uvm_ce_hal_t) / sizeof(void *))
#define HOST_OP_COUNT (sizeof(uvm_host_hal_t) / sizeof(void *))
#define ARCH_OP_COUNT (sizeof(uvm_arch_hal_t) / sizeof(void *))
#define FAULT_BUFFER_OP_COUNT (sizeof(uvm_fault_buffer_hal_t) / sizeof(void *))

// Table for copy engine functions.
// Each entry is associated with a copy engine class through the 'class' field.
// By setting the 'parent_class' field, a class will inherit the parent class's
// functions for any fields left NULL when uvm_hal_init_table() runs upon module load.
// The parent class must appear earlier in the array than the child.
static uvm_hal_class_ops_t ce_table_base[] =
{
    {
        .id = KEPLER_DMA_COPY_A,
        .u.ce_ops = {
            .init = uvm_hal_kepler_ce_init,
            .semaphore_release = uvm_hal_kepler_ce_semaphore_release,
            .semaphore_timestamp = uvm_hal_kepler_ce_semaphore_timestamp,
            .semaphore_reduction_inc = uvm_hal_kepler_ce_semaphore_reduction_inc,
            .offset_out = uvm_hal_kepler_ce_offset_out,
            .offset_in_out = uvm_hal_kepler_ce_offset_in_out,
            .memcopy = uvm_hal_kepler_ce_memcopy,
            .memcopy_v_to_v = uvm_hal_kepler_ce_memcopy_v_to_v,
            .memset_1 = uvm_hal_kepler_ce_memset_1,
            .memset_4 = uvm_hal_kepler_ce_memset_4,
            .memset_8 = uvm_hal_kepler_ce_memset_8,
            .memset_v_4 = uvm_hal_kepler_ce_memset_v_4,
        }
    },
    {
        .id = MAXWELL_DMA_COPY_A,
        .parent_id = KEPLER_DMA_COPY_A,
        .u.ce_ops = {}
    },
    {
        .id = PASCAL_DMA_COPY_A,
        .parent_id = MAXWELL_DMA_COPY_A,
        .u.ce_ops = {
            .offset_out = uvm_hal_pascal_ce_offset_out,
            .offset_in_out = uvm_hal_pascal_ce_offset_in_out,
        }
    },
    {
        .id = PASCAL_DMA_COPY_B,
        .parent_id = PASCAL_DMA_COPY_A,
        .u.ce_ops = {}
    },
};

// Table for GPFIFO functions.  Same idea as the copy engine table.
static uvm_hal_class_ops_t host_table_base[] =
{
    {
        .id = KEPLER_CHANNEL_GPFIFO_A,
        .u.host_ops = {
            .init = uvm_hal_kepler_host_init_noop,
            .wait_for_idle = uvm_hal_kepler_host_wait_for_idle_a06f,
            .membar_sys = uvm_hal_kepler_host_membar_sys,
            // No MEMBAR GPU until Pascal, just do a MEMBAR SYS.
            .membar_gpu = uvm_hal_kepler_host_membar_sys,
            .noop = uvm_hal_kepler_host_noop,
            .interrupt = uvm_hal_kepler_host_interrupt,
            .semaphore_acquire = uvm_hal_kepler_host_semaphore_acquire,
            .semaphore_release = uvm_hal_kepler_host_semaphore_release,
            .set_gpfifo_entry = uvm_hal_kepler_host_set_gpfifo_entry,
            .write_gpu_put = uvm_hal_kepler_host_write_gpu_put,
            .tlb_invalidate_all = uvm_hal_kepler_host_tlb_invalidate_all,
            .tlb_invalidate_va = uvm_hal_kepler_host_tlb_invalidate_va,
            .tlb_invalidate_test = uvm_hal_kepler_host_tlb_invalidate_test,
            .replay_faults = uvm_hal_kepler_replay_faults_unsupported,
            .cancel_faults_targeted = uvm_hal_kepler_cancel_faults_targeted_unsupported,
        }
    },
    {
        .id = KEPLER_CHANNEL_GPFIFO_B,
        .parent_id = KEPLER_CHANNEL_GPFIFO_A,
        .u.host_ops = {
            .wait_for_idle = uvm_hal_kepler_host_wait_for_idle_a16f,
        }
    },
    {
        .id = MAXWELL_CHANNEL_GPFIFO_A,
        .parent_id = KEPLER_CHANNEL_GPFIFO_A,
        .u.host_ops = {
            .tlb_invalidate_all = uvm_hal_maxwell_host_tlb_invalidate_all,
        }
    },
    {
        .id = PASCAL_CHANNEL_GPFIFO_A,
        .parent_id = MAXWELL_CHANNEL_GPFIFO_A,
        .u.host_ops = {
            .init = uvm_hal_pascal_host_init,
            .membar_sys = uvm_hal_pascal_host_membar_sys,
            .membar_gpu = uvm_hal_pascal_host_membar_gpu,
            .tlb_invalidate_all = uvm_hal_pascal_host_tlb_invalidate_all,
            .tlb_invalidate_va = uvm_hal_pascal_host_tlb_invalidate_va,
            .tlb_invalidate_test = uvm_hal_pascal_host_tlb_invalidate_test,
            .replay_faults = uvm_hal_pascal_replay_faults,
            .cancel_faults_targeted = uvm_hal_pascal_cancel_faults_targeted,
        }
    },
};

static uvm_hal_class_ops_t arch_table_base[] =
{
    {
        .id = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GK100,
        .u.arch_ops = {
            .init_properties = uvm_hal_kepler_arch_init_properties,
            .mmu_mode_hal = uvm_hal_mmu_mode_kepler,
            .enable_prefetch_faults = uvm_hal_kepler_mmu_enable_prefetch_faults_unsupported,
            .disable_prefetch_faults = uvm_hal_kepler_mmu_disable_prefetch_faults_unsupported,
        }
    },
    {
        .id = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GK110,
        .parent_id = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GK100,
        .u.arch_ops = {}
    },
    {
        .id = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GK200,
        .parent_id = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GK100,
        .u.arch_ops = {}
    },
    {
        .id = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GM000,
        .parent_id = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GK100,
        .u.arch_ops = {
            .init_properties = uvm_hal_maxwell_arch_init_properties,
        }
    },
    {
        .id = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GM200,
        .parent_id = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GM000,
        .u.arch_ops = {}
    },
    {
        .id = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GP100,
        .parent_id = NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GM000,
        .u.arch_ops = {
            .init_properties = uvm_hal_pascal_arch_init_properties,
            .mmu_mode_hal = uvm_hal_mmu_mode_pascal,
            .enable_prefetch_faults = uvm_hal_pascal_mmu_enable_prefetch_faults,
            .disable_prefetch_faults = uvm_hal_pascal_mmu_disable_prefetch_faults,
        }
    }
};

static uvm_hal_class_ops_t fault_buffer_table_base[] =
{
    {
        .id = MAXWELL_FAULT_BUFFER_A,
        .u.fault_buffer_ops = {
            .enable_replayable_faults  = uvm_hal_pascal_enable_replayable_faults,
            .disable_replayable_faults = uvm_hal_pascal_disable_replayable_faults,
            .parse_entry = uvm_hal_pascal_fault_buffer_parse_entry,
            .entry_is_valid = uvm_hal_pascal_fault_buffer_entry_is_valid,
            .entry_clear_valid = uvm_hal_pascal_fault_buffer_entry_clear_valid,
            .entry_size = uvm_hal_pascal_fault_buffer_entry_size,
        }
    }
};

// Dynamically allocated tables with the entries from base and next combined.
static uvm_hal_class_ops_t *ce_table_all;
static uvm_hal_class_ops_t *host_table_all;
static uvm_hal_class_ops_t *arch_table_all;
static uvm_hal_class_ops_t *fault_buffer_table_all;

static inline uvm_hal_class_ops_t *ops_find_by_id(uvm_hal_class_ops_t *table, NvU32 row_count, NvU32 id)
{
    NvLength i;

    // go through array and match on class.
    for (i = 0; i < row_count; i++) {
        if (table[i].id == id)
            return table + i;
    }

    return NULL;
}

// use memcmp to check for function pointer assignment in a well defined, general way.
static inline bool op_is_null(uvm_hal_class_ops_t *row, NvLength op_idx, NvLength op_offset)
{
    void *temp = NULL;
    return memcmp(&temp, (char *)row + op_offset + sizeof(void *) * op_idx, sizeof(void *)) == 0;
}

// use memcpy to copy function pointers in a well defined, general way.
static inline void op_copy(uvm_hal_class_ops_t *dst, uvm_hal_class_ops_t *src, NvLength op_idx, NvLength op_offset)
{
    void *m_dst = (char *)dst + op_offset + sizeof(void *) * op_idx;
    void *m_src = (char *)src + op_offset + sizeof(void *) * op_idx;
    memcpy(m_dst, m_src, sizeof(void *));
}

static inline NV_STATUS ops_init_from_parent(uvm_hal_class_ops_t *table,
                                             NvU32 row_count,
                                             NvLength op_count,
                                             NvLength op_offset)
{
    NvLength i;

    for (i = 0; i < row_count; i++) {
        NvLength j;
        uvm_hal_class_ops_t *parent = NULL;

        if (table[i].parent_id != 0) {
            parent = ops_find_by_id(table, i, table[i].parent_id);
            if (parent == NULL)
                return NV_ERR_INVALID_CLASS;

            // Go through all the ops and assign from parent's corresponding op if NULL
            for (j = 0; j < op_count; j++) {
                if (op_is_null(table + i, j, op_offset))
                    op_copy(table + i, parent, j, op_offset);
            }
        }

        // At this point, it is an error to have missing HAL operations
        for (j = 0; j < op_count; j++) {
            if (op_is_null(table + i, j, op_offset))
                return NV_ERR_INVALID_STATE;
        }
    }

    return NV_OK;
}

static uvm_hal_class_ops_t *combine_tables(uvm_hal_class_ops_t *base_table,
                                           size_t base_table_count,
                                           uvm_hal_class_ops_t *next_table,
                                           size_t next_table_count)
{
    size_t total_count = base_table_count + next_table_count;
    uvm_hal_class_ops_t *combined_table = uvm_kvmalloc(total_count * sizeof(*base_table));
    if (!combined_table)
        return NULL;

    memcpy(combined_table, base_table, base_table_count * sizeof(*base_table));
    memcpy(combined_table + base_table_count, next_table, next_table_count * sizeof(*base_table));

    return combined_table;
}

static NV_STATUS uvm_hal_init_combine_tables(void)
{
    ce_table_all = combine_tables(ce_table_base, ce_class_count_base(),
                                  ce_table_next, ce_class_count_next());
    if (!ce_table_all)
        return NV_ERR_NO_MEMORY;

    host_table_all = combine_tables(host_table_base, host_class_count_base(),
                                    host_table_next, host_class_count_next());
    if (!host_table_all)
        return NV_ERR_NO_MEMORY;

    arch_table_all = combine_tables(arch_table_base, arch_class_count_base(),
                                    arch_table_next, arch_class_count_next());
    if (!arch_table_all)
        return NV_ERR_NO_MEMORY;

    fault_buffer_table_all = combine_tables(fault_buffer_table_base, fault_buffer_class_count_base(),
                                            fault_buffer_table_next, fault_buffer_class_count_next());
    if (!fault_buffer_table_all)
        return NV_ERR_NO_MEMORY;

    return NV_OK;
}

NV_STATUS uvm_hal_init_table(void)
{
    NV_STATUS status;
    status = uvm_hal_init_combine_tables();
    if (status != NV_OK) {
        UVM_ERR_PRINT("Failed to combine hal tables: %s\n", nvstatusToString(status));
        return status;
    }

    status = ops_init_from_parent(ce_table_all, ce_class_count(), CE_OP_COUNT, offsetof(uvm_hal_class_ops_t, u.ce_ops));
    if (status != NV_OK) {
        UVM_ERR_PRINT("ops_init_from_parent(ce_table) failed: %s\n", nvstatusToString(status));
        return status;
    }

    status = ops_init_from_parent(host_table_all, host_class_count(), HOST_OP_COUNT, offsetof(uvm_hal_class_ops_t, u.host_ops));
    if (status != NV_OK) {
        UVM_ERR_PRINT("ops_init_from_parent(host_table) failed: %s\n", nvstatusToString(status));
        return status;
    }

    status = ops_init_from_parent(arch_table_all, arch_class_count(), ARCH_OP_COUNT, offsetof(uvm_hal_class_ops_t, u.arch_ops));
    if (status != NV_OK) {
        UVM_ERR_PRINT("ops_init_from_parent(arch_table) failed: %s\n", nvstatusToString(status));
        return status;
    }

    status = ops_init_from_parent(fault_buffer_table_all, fault_buffer_class_count(), FAULT_BUFFER_OP_COUNT,
                                  offsetof(uvm_hal_class_ops_t, u.fault_buffer_ops));
    if (status != NV_OK) {
        UVM_ERR_PRINT("ops_init_from_parent(fault_buffer_table) failed: %s\n", nvstatusToString(status));
        return status;
    }


    return NV_OK;
}

void uvm_hal_free_table(void)
{
    uvm_kvfree(fault_buffer_table_all);
    uvm_kvfree(arch_table_all);
    uvm_kvfree(host_table_all);
    uvm_kvfree(ce_table_all);
}

NV_STATUS uvm_hal_init_gpu(uvm_gpu_t *gpu)
{
    uvm_hal_class_ops_t *class_ops = ops_find_by_id(ce_table_all, ce_class_count(), gpu->ce_class);
    if (class_ops == NULL) {
        UVM_ERR_PRINT("Unsupported ce class: 0x%X, GPU %s\n", gpu->ce_class, gpu->name);
        return NV_ERR_INVALID_CLASS;
    }

    gpu->ce_hal = &class_ops->u.ce_ops;

    class_ops = ops_find_by_id(host_table_all, host_class_count(), gpu->host_class);
    if (class_ops == NULL) {
        UVM_ERR_PRINT("Unsupported host class: 0x%X, GPU %s\n", gpu->host_class, gpu->name);
        return NV_ERR_INVALID_CLASS;
    }

    gpu->host_hal = &class_ops->u.host_ops;

    class_ops = ops_find_by_id(arch_table_all, arch_class_count(), gpu->architecture);
    if (class_ops == NULL) {
        UVM_ERR_PRINT("Unsupported GPU architecture: 0x%X, GPU %s\n", gpu->architecture, gpu->name);
        return NV_ERR_INVALID_CLASS;
    }

    gpu->arch_hal = &class_ops->u.arch_ops;

    // Initialize the fault buffer hal only for GPUs supporting faults (with non-0 fault buffer class).
    if (gpu->fault_buffer_class != 0) {
        class_ops = ops_find_by_id(fault_buffer_table_all, fault_buffer_class_count(), gpu->fault_buffer_class);
        if (class_ops == NULL) {
            UVM_ERR_PRINT("Unsupported fault buffer class: 0x%X, GPU %s\n", gpu->fault_buffer_class, gpu->name);
            return NV_ERR_INVALID_CLASS;
        }
        gpu->fault_buffer_hal = &class_ops->u.fault_buffer_ops;
    }
    else {
        gpu->fault_buffer_hal = NULL;
    }

    return NV_OK;
}

bool uvm_hal_fault_buffer_class_supports_replayable_faults(NvU32 fault_buffer_class)
{
    return fault_buffer_class != 0;
}

const char *uvm_aperture_string(uvm_aperture_t aperture)
{
    BUILD_BUG_ON(UVM_APERTURE_MAX != 12);

    switch (aperture) {
        UVM_ENUM_STRING_CASE(UVM_APERTURE_PEER_0);
        UVM_ENUM_STRING_CASE(UVM_APERTURE_PEER_1);
        UVM_ENUM_STRING_CASE(UVM_APERTURE_PEER_2);
        UVM_ENUM_STRING_CASE(UVM_APERTURE_PEER_3);
        UVM_ENUM_STRING_CASE(UVM_APERTURE_PEER_4);
        UVM_ENUM_STRING_CASE(UVM_APERTURE_PEER_5);
        UVM_ENUM_STRING_CASE(UVM_APERTURE_PEER_6);
        UVM_ENUM_STRING_CASE(UVM_APERTURE_PEER_7);
        UVM_ENUM_STRING_CASE(UVM_APERTURE_PEER_MAX);
        UVM_ENUM_STRING_CASE(UVM_APERTURE_SYS);
        UVM_ENUM_STRING_CASE(UVM_APERTURE_VID);
        UVM_ENUM_STRING_CASE(UVM_APERTURE_DEFAULT);
        UVM_ENUM_STRING_DEFAULT();
    }
}

const char *uvm_prot_string(uvm_prot_t prot)
{
    BUILD_BUG_ON(UVM_PROT_MAX != 4);

    switch (prot) {
        UVM_ENUM_STRING_CASE(UVM_PROT_NONE);
        UVM_ENUM_STRING_CASE(UVM_PROT_READ_ONLY);
        UVM_ENUM_STRING_CASE(UVM_PROT_READ_WRITE);
        UVM_ENUM_STRING_CASE(UVM_PROT_READ_WRITE_ATOMIC);
        UVM_ENUM_STRING_DEFAULT();
    }
}

const char *uvm_membar_string(uvm_membar_t membar)
{
    switch (membar) {
        UVM_ENUM_STRING_CASE(UVM_MEMBAR_SYS);
        UVM_ENUM_STRING_CASE(UVM_MEMBAR_GPU);
        UVM_ENUM_STRING_CASE(UVM_MEMBAR_NONE);
    }

    return "UNKNOWN";
}

const char *uvm_fault_access_type_string(uvm_fault_access_type_t fault_access_type)
{
    BUILD_BUG_ON(UVM_FAULT_ACCESS_TYPE_MAX != 4);

    switch (fault_access_type) {
        UVM_ENUM_STRING_CASE(UVM_FAULT_ACCESS_TYPE_ATOMIC);
        UVM_ENUM_STRING_CASE(UVM_FAULT_ACCESS_TYPE_WRITE);
        UVM_ENUM_STRING_CASE(UVM_FAULT_ACCESS_TYPE_READ);
        UVM_ENUM_STRING_CASE(UVM_FAULT_ACCESS_TYPE_PREFETCH);
        UVM_ENUM_STRING_DEFAULT();
    }
}

const char *uvm_fault_type_string(uvm_fault_type_t fault_type)
{
    BUILD_BUG_ON(UVM_FAULT_TYPE_MAX != 16);

    switch (fault_type) {
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_INVALID_PDE);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_INVALID_PTE);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_ATOMIC);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_WRITE);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_READ);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_PDE_SIZE);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_VA_LIMIT_VIOLATION);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_UNBOUND_INST_BLOCK);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_PRIV_VIOLATION);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_PITCH_MASK_VIOLATION);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_WORK_CREATION);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_UNSUPPORTED_APERTURE);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_COMPRESSION_FAILURE);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_UNSUPPORTED_KIND);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_REGION_VIOLATION);
        UVM_ENUM_STRING_CASE(UVM_FAULT_TYPE_POISONED);
        UVM_ENUM_STRING_DEFAULT();
    }
}

const char *uvm_fault_client_type_string(uvm_fault_client_type_t fault_client_type)
{
    BUILD_BUG_ON(UVM_FAULT_CLIENT_TYPE_MAX != 2);

    switch (fault_client_type) {
        UVM_ENUM_STRING_CASE(UVM_FAULT_CLIENT_TYPE_GPC);
        UVM_ENUM_STRING_CASE(UVM_FAULT_CLIENT_TYPE_HUB);
        UVM_ENUM_STRING_DEFAULT();
    }
}

void uvm_hal_print_fault_entry(uvm_fault_buffer_entry_t *entry)
{
    UVM_DBG_PRINT("fault_address:                  %p\n", (void *)entry->fault_address);
    UVM_DBG_PRINT("    fault_instance_ptr:         {%s, %p}\n", uvm_aperture_string(entry->instance_ptr.aperture),
                                                                (void *)entry->instance_ptr.address);
    UVM_DBG_PRINT("    fault_type:                 %s\n", uvm_fault_type_string(entry->fault_type));
    UVM_DBG_PRINT("    fault_access_type:          %s\n", uvm_fault_access_type_string(entry->fault_access_type));
    UVM_DBG_PRINT("    fault_source.client_type:   %s\n", uvm_fault_client_type_string(entry->fault_source.client_type));
    UVM_DBG_PRINT("    fault_source.client_id:     %d\n", entry->fault_source.client_id);
    UVM_DBG_PRINT("    fault_source.gpc_id:        %d\n", entry->fault_source.gpc_id);
    UVM_DBG_PRINT("    timestamp:                  %llu\n", entry->timestamp);
    uvm_hal_print_next_fault_entry_fields(entry);
}
