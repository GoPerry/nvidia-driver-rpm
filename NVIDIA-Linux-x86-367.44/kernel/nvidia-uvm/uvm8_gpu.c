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

#include "nv_uvm_interface.h"
#include "uvm8_api.h"
#include "uvm8_channel.h"
#include "uvm8_global.h"
#include "uvm8_gpu.h"
#include "uvm8_next_decl.h"
#include "uvm8_gpu_semaphore.h"
#include "uvm8_hal.h"
#include "uvm8_procfs.h"
#include "uvm8_pmm_gpu.h"
#include "uvm8_va_space.h"
#include "uvm8_gpu_page_fault.h"
#include "uvm8_user_channel.h"
#include "uvm8_perf_events.h"
#include "uvm_common.h"
#include "ctrl2080mc.h"
#include "nv-kthread-q.h"

static void remove_gpu(uvm_gpu_t *gpu);
static void disable_peer_access(uvm_gpu_t *gpu_1, uvm_gpu_t *gpu_2);

static NV_STATUS get_gpu_info(uvm_gpu_t *gpu)
{
    NV_STATUS status;
    UvmGpuInfo gpu_info = {{0}};
    char uuid_buffer[UVM_GPU_UUID_TEXT_BUFFER_LENGTH];

    status = uvm_rm_locked_call(nvUvmInterfaceGetGpuInfo(&gpu->uuid, &gpu_info));
    if (status != NV_OK)
        return status;

    gpu->architecture = gpu_info.gpuArch;
    gpu->implementation = gpu_info.gpuImplementation;

    gpu->host_class = gpu_info.hostClass;
    gpu->ce_class = gpu_info.ceClass;
    gpu->fault_buffer_class = gpu_info.faultBufferClass;

    gpu->sli_enabled = (gpu_info.subdeviceCount > 1);

    gpu->is_simulated = gpu_info.isSimulated;

    format_uuid_to_buffer(uuid_buffer, sizeof(uuid_buffer), &gpu->uuid);
    snprintf(gpu->name, sizeof(gpu->name), "ID %u: %s: %s", gpu->id, gpu_info.name, uuid_buffer);

    return status;
}

static NV_STATUS get_gpu_caps(uvm_gpu_t *gpu)
{
    NV_STATUS status;
    UvmGpuCaps gpu_caps = {0};
    UvmGpuFbInfo fb_info = {0};

    status = uvm_rm_locked_call(nvUvmInterfaceGetBigPageSize(gpu->rm_address_space, &gpu->big_page.internal_size));
    if (status != NV_OK)
        return status;

    status = uvm_rm_locked_call(nvUvmInterfaceQueryCaps(gpu->rm_address_space, &gpu_caps));
    if (status != NV_OK)
        return status;

    status = uvm_rm_locked_call(nvUvmInterfaceGetFbInfo(gpu->rm_address_space, &fb_info));
    if (status != NV_OK)
        return status;

    gpu->vidmem_size = ((NvU64)fb_info.heapSize + fb_info.reservedHeapSize) * 1024;
    gpu->vidmem_max_physical_address = fb_info.maxPhysicalAddress;

    memcpy(gpu->ce_caps, gpu_caps.copyEngineCaps, sizeof(gpu->ce_caps));

    gpu->ecc.enabled = gpu_caps.bEccEnabled;
    if (gpu->ecc.enabled) {
        gpu->ecc.hw_interrupt_tree_location = (volatile NvU32*)((char*)gpu_caps.eccReadLocation + gpu_caps.eccOffset);
        UVM_ASSERT(gpu->ecc.hw_interrupt_tree_location != NULL);
        gpu->ecc.mask = gpu_caps.eccMask;
        UVM_ASSERT(gpu->ecc.mask != 0);

        gpu->ecc.error_notifier = gpu_caps.eccErrorNotifier;
        UVM_ASSERT(gpu->ecc.error_notifier != NULL);
    }

    return NV_OK;
}

static bool gpu_supports_uvm(uvm_gpu_t *gpu)
{
    // TODO: Bug 1757136: Add Linux SLI support. Until then, explicitly disable
    //       UVM on SLI.
    return !gpu->sli_enabled && gpu->architecture >= NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_GK100;
}

bool uvm_gpu_can_address(uvm_gpu_t *gpu, NvU64 addr)
{
    NvU64 max_va;

    // Watch out for calling this too early in init
    UVM_ASSERT(gpu->address_space_tree.hal);
    UVM_ASSERT(gpu->address_space_tree.hal->num_va_bits() < 64);
    max_va = 1ULL << gpu->address_space_tree.hal->num_va_bits();

    // Despite not supporting a full 64-bit VA space, Pascal+ GPUs are capable
    // of accessing kernel pointers in various modes by applying the same upper-
    // bit checks that x86, ARM, and and Power processors do. We don't have an
    // immediate use case for that so we'll just let the below check fail if
    // addr falls in the upper bits which belong to kernel space.
    return addr < max_va;
}

static void
gpu_info_print_common(uvm_gpu_t *gpu, struct seq_file *s)
{
    NvU32 i;
    NvU64 num_pages_in;
    NvU64 num_pages_out;

    UVM_SEQ_OR_DBG_PRINT(s, "GPU %s\n", gpu->name);
    UVM_SEQ_OR_DBG_PRINT(s, "retained_count        %llu\n", uvm_gpu_retained_count(gpu));
    UVM_SEQ_OR_DBG_PRINT(s, "ecc                   %s\n", gpu->ecc.enabled ? "enabled" : "disabled");

    if (!uvm_procfs_is_debug_enabled())
        return;

    UVM_SEQ_OR_DBG_PRINT(s, "architecture          0x%X\n", gpu->architecture);
    UVM_SEQ_OR_DBG_PRINT(s, "implementation        0x%X\n", gpu->implementation);
    UVM_SEQ_OR_DBG_PRINT(s, "host_class            0x%X\n", gpu->host_class);
    UVM_SEQ_OR_DBG_PRINT(s, "ce_class              0x%X\n", gpu->ce_class);
    UVM_SEQ_OR_DBG_PRINT(s, "fault_buffer_class    0x%X\n", gpu->fault_buffer_class);
    UVM_SEQ_OR_DBG_PRINT(s, "big_page_size         %u\n", gpu->big_page.internal_size);
    UVM_SEQ_OR_DBG_PRINT(s, "big_page_swizzling    %u\n", gpu->big_page.swizzling ? 1 : 0);
    UVM_SEQ_OR_DBG_PRINT(s, "rm_va_base            0x%llx\n", gpu->rm_va_base);
    UVM_SEQ_OR_DBG_PRINT(s, "rm_va_size            0x%llx\n", gpu->rm_va_size);
    UVM_SEQ_OR_DBG_PRINT(s, "vidmem_size           %llu (%llu MBs)\n", gpu->vidmem_size,
                                                                       gpu->vidmem_size / (1024 * 1024));
    UVM_SEQ_OR_DBG_PRINT(s, "vidmem_max_physical   0x%llx (%llu MBs)\n", gpu->vidmem_max_physical_address,
                                                                         gpu->vidmem_max_physical_address /
                                                                         (1024 * 1024));
    UVM_SEQ_OR_DBG_PRINT(s, "interrupts            %llu\n", gpu->interrupt_count);
    UVM_SEQ_OR_DBG_PRINT(s, "bottom_halves         %llu\n", gpu->interrupt_count_bottom_half);

    if (gpu->handling_replayable_faults) {
        UVM_SEQ_OR_DBG_PRINT(s, "fault_buffer_entries  %u\n", gpu->fault_buffer_info.max_faults);
        UVM_SEQ_OR_DBG_PRINT(s, "cached_get            %u\n", gpu->fault_buffer_info.replayable.cached_get);
        UVM_SEQ_OR_DBG_PRINT(s, "cached_put            %u\n", gpu->fault_buffer_info.replayable.cached_put);
        UVM_SEQ_OR_DBG_PRINT(s, "fault_batch_size      %u\n", gpu->fault_buffer_info.fault_batch_count);
        UVM_SEQ_OR_DBG_PRINT(s, "replay_policy         %s\n",
                             uvm_perf_fault_replay_policy_string(gpu->fault_buffer_info.replayable.replay_policy));
        UVM_SEQ_OR_DBG_PRINT(s, "faults                %llu\n", gpu->stats.num_faults);
    }

    num_pages_out = atomic64_read(&gpu->stats.num_pages_out);
    num_pages_in = atomic64_read(&gpu->stats.num_pages_in);

    UVM_SEQ_OR_DBG_PRINT(s, "migrated_pages_in     %llu (%llu MB)\n", num_pages_in,
                         (num_pages_in * (NvU64)PAGE_SIZE) / (1024u * 1024u));
    UVM_SEQ_OR_DBG_PRINT(s, "migrated_pages_out    %llu (%llu MB)\n", num_pages_out,
                         (num_pages_out * (NvU64)PAGE_SIZE) / (1024u * 1024u));

    UVM_SEQ_OR_DBG_PRINT(s, "supported_ces:\n");
    for (i = 0; i < UVM_COPY_ENGINE_COUNT_MAX; ++i) {
        if (!gpu->ce_caps[i].supported)
            continue;
        UVM_SEQ_OR_DBG_PRINT(s, " ce %u grce %u shared %u sysmem read %u sysmem write %u"
                                " sysmem %u nvlink p2p %u p2p %u\n",
                i,
                gpu->ce_caps[i].grce,
                gpu->ce_caps[i].shared,
                gpu->ce_caps[i].sysmemRead,
                gpu->ce_caps[i].sysmemWrite,
                gpu->ce_caps[i].sysmem,
                gpu->ce_caps[i].nvlinkP2p,
                gpu->ce_caps[i].p2p);
    }
}

static void
gpu_fault_stats_print_common(uvm_gpu_t *gpu, struct seq_file *s)
{
    NvU64 num_pages_in;
    NvU64 num_pages_out;

    if (!uvm_procfs_is_debug_enabled())
        return;

    num_pages_out = atomic64_read(&gpu->fault_buffer_info.replayable.stats.num_pages_out);
    num_pages_in = atomic64_read(&gpu->fault_buffer_info.replayable.stats.num_pages_in);

    UVM_SEQ_OR_DBG_PRINT(s, "faults_by_access_type:\n");
    UVM_SEQ_OR_DBG_PRINT(s, "  prefetch             %llu\n", gpu->fault_buffer_info.replayable.stats.num_prefetch_faults);
    UVM_SEQ_OR_DBG_PRINT(s, "  read                 %llu\n", gpu->fault_buffer_info.replayable.stats.num_read_faults);
    UVM_SEQ_OR_DBG_PRINT(s, "  write                %llu\n", gpu->fault_buffer_info.replayable.stats.num_write_faults);
    UVM_SEQ_OR_DBG_PRINT(s, "  atomics              %llu\n", gpu->fault_buffer_info.replayable.stats.num_atomic_faults);
    UVM_SEQ_OR_DBG_PRINT(s, "migrations:\n");
    UVM_SEQ_OR_DBG_PRINT(s, "  num_pages_in         %llu (%llu MB)\n", num_pages_in,
                         (num_pages_in * (NvU64)PAGE_SIZE) / (1024u * 1024u));
    UVM_SEQ_OR_DBG_PRINT(s, "  num_pages_out        %llu (%llu MB)\n", num_pages_out,
                         (num_pages_out * (NvU64)PAGE_SIZE) / (1024u * 1024u));
    UVM_SEQ_OR_DBG_PRINT(s, "replays:\n");
    UVM_SEQ_OR_DBG_PRINT(s, "  start                %llu\n", gpu->fault_buffer_info.replayable.stats.num_replays);
    UVM_SEQ_OR_DBG_PRINT(s, "  start_ack_all        %llu\n", gpu->fault_buffer_info.replayable.stats.num_replays_ack_all);
}

void uvm_gpu_print(uvm_gpu_t *gpu)
{
    gpu_info_print_common(gpu, NULL);
}

static int
nv_procfs_read_gpu_info(struct seq_file *s, void *v)
{
    uvm_gpu_t *gpu = (uvm_gpu_t *)s->private;
    gpu_info_print_common(gpu, s);
    return 0;
}

static int
nv_procfs_read_gpu_fault_stats(struct seq_file *s, void *v)
{
    uvm_gpu_t *gpu = (uvm_gpu_t *)s->private;
    gpu_fault_stats_print_common(gpu, s);
    return 0;
}

NV_DEFINE_PROCFS_SINGLE_FILE(gpu_info);
NV_DEFINE_PROCFS_SINGLE_FILE(gpu_fault_stats);

static NV_STATUS init_procfs_dirs(uvm_gpu_t *gpu)
{
    // This needs to hold a gpu_id_t in decimal
    char gpu_dir_name[16];

    // This needs to hold a GPU UUID
    char symlink_name[UVM_GPU_UUID_TEXT_BUFFER_LENGTH];

    struct proc_dir_entry *gpu_base_dir_entry;
    if (!uvm_procfs_is_enabled())
        return NV_OK;

    gpu_base_dir_entry = uvm_procfs_get_gpu_base_dir();

    snprintf(gpu_dir_name, sizeof(gpu_dir_name), "%u", gpu->id);
    gpu->procfs.dir = NV_CREATE_PROC_DIR(gpu_dir_name, gpu_base_dir_entry);
    if (gpu->procfs.dir == NULL)
        return NV_ERR_OPERATING_SYSTEM;

    // Create a symlink from UVM GPU UUID (UVM-GPU-...) to the UVM GPU ID
    format_uuid_to_buffer(symlink_name, sizeof(symlink_name), &gpu->uuid);
    gpu->procfs.dir_uuid_symlink = proc_symlink(symlink_name, gpu_base_dir_entry, gpu_dir_name);
    if (gpu->procfs.dir_uuid_symlink == NULL)
        return NV_ERR_OPERATING_SYSTEM;

    return NV_OK;
}

static void deinit_procfs_dirs(uvm_gpu_t *gpu)
{
    uvm_procfs_destroy_entry(gpu->procfs.dir_uuid_symlink);
    uvm_procfs_destroy_entry(gpu->procfs.dir);
}

static NV_STATUS init_procfs_files(uvm_gpu_t *gpu)
{
    gpu->procfs.info_file = NV_CREATE_PROC_FILE("info", gpu->procfs.dir, gpu_info, (void *)gpu);
    if (gpu->procfs.info_file == NULL)
        return NV_ERR_OPERATING_SYSTEM;

    gpu->procfs.fault_stats_file = NV_CREATE_PROC_FILE("fault_stats", gpu->procfs.dir, gpu_fault_stats, (void *)gpu);
    if (gpu->procfs.fault_stats_file == NULL)
        return NV_ERR_OPERATING_SYSTEM;

    return NV_OK;
}

static void deinit_procfs_files(uvm_gpu_t *gpu)
{
    uvm_procfs_destroy_entry(gpu->procfs.info_file);
    uvm_procfs_destroy_entry(gpu->procfs.fault_stats_file);
}

static NV_STATUS init_semaphore_pool(uvm_gpu_t *gpu)
{
    NV_STATUS status;
    uvm_gpu_t *other_gpu;

    status = uvm_gpu_semaphore_pool_create(gpu, &gpu->semaphore_pool);
    if (status != NV_OK)
        return status;

    for_each_global_gpu(other_gpu) {
        if (other_gpu == gpu)
            continue;
        status = uvm_gpu_semaphore_pool_map_gpu(other_gpu->semaphore_pool, gpu);
        if (status != NV_OK)
            return status;
    }

    return NV_OK;
}

static void deinit_semaphore_pool(uvm_gpu_t *gpu)
{
    uvm_gpu_t *other_gpu;

    for_each_global_gpu(other_gpu) {
        if (other_gpu == gpu)
            continue;
        uvm_gpu_semaphore_pool_unmap_gpu(other_gpu->semaphore_pool, gpu);
    }

    uvm_gpu_semaphore_pool_destroy(gpu->semaphore_pool);
}

// Allocates a uvm_gpu_t*, assigns a gpu->id to it, but leaves all other initialization up to
// the caller.
static NV_STATUS alloc_gpu(NvProcessorUuid *gpu_uuid, uvm_gpu_t **gpu_out)
{
    uvm_gpu_t *gpu;
    uvm_gpu_id_t id;
    uvm_gpu_id_t new_gpu_id;
    bool found_a_slot = false;

    uvm_assert_mutex_locked(&g_uvm_global.global_lock);

    // Find an unused slot:
    for_each_gpu_id(id) {
        gpu = uvm_gpu_get(id);

        if (gpu == NULL) {
            new_gpu_id = id;
            found_a_slot = true;
            break;
        }
    }

    if (!found_a_slot)
        return NV_ERR_INSUFFICIENT_RESOURCES;

    gpu = uvm_kvmalloc_zero(sizeof(*gpu));
    if (!gpu)
        return NV_ERR_NO_MEMORY;

    gpu->id = new_gpu_id;

    // Initialize enough of the gpu struct for remove_gpu to be called
    gpu->magic = UVM_GPU_MAGIC_VALUE;
    uvm_processor_uuid_copy(&gpu->uuid, gpu_uuid);
    uvm_mutex_init(&gpu->isr_lock, UVM_LOCK_ORDER_ISR);
    uvm_spin_lock_irqsave_init(&gpu->page_fault_interrupts_lock, UVM_LOCK_ORDER_LEAF);
    uvm_spin_lock_init(&gpu->instance_ptr_table_lock, UVM_LOCK_ORDER_LEAF);
    uvm_init_radix_tree_preloadable(&gpu->instance_ptr_table);
    uvm_mutex_init(&gpu->big_page.staging.lock, UVM_LOCK_ORDER_SWIZZLE_STAGING);
    uvm_tracker_init(&gpu->big_page.staging.tracker);

    kref_init(&gpu->gpu_kref);

    *gpu_out = gpu;

    return NV_OK;
}

static NV_STATUS configure_address_space(uvm_gpu_t *gpu)
{
    NV_STATUS status;
    NvU32 num_entries;
    NvU64 va_size;
    NvU64 va_per_entry;

    status = uvm_page_tree_init(gpu, gpu->big_page.internal_size, UVM_APERTURE_DEFAULT, &gpu->address_space_tree);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Initializing the page tree failed: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        return status;
    }

    num_entries = uvm_mmu_page_tree_entries(&gpu->address_space_tree, 0, UVM_PAGE_SIZE_AGNOSTIC);

    UVM_ASSERT(gpu->address_space_tree.hal->num_va_bits() < 64);
    va_size = 1ull << gpu->address_space_tree.hal->num_va_bits();
    va_per_entry = va_size / num_entries;

    // Make sure that RM's part of the VA is aligned to the VA covered by a
    // single top level PDE.
    UVM_ASSERT_MSG(gpu->rm_va_base % va_per_entry == 0,
            "va_base 0x%llx va_per_entry 0x%llx\n", gpu->rm_va_base, va_per_entry);
    UVM_ASSERT_MSG(gpu->rm_va_size % va_per_entry == 0,
            "va_size 0x%llx va_per_entry 0x%llx\n", gpu->rm_va_size, va_per_entry);

    status = uvm_rm_locked_call(nvUvmInterfaceSetPageDirectory(gpu->rm_address_space,
            uvm_page_tree_pdb(&gpu->address_space_tree)->addr.address, num_entries,
            uvm_page_tree_pdb(&gpu->address_space_tree)->addr.aperture == UVM_APERTURE_VID));
    if (status != NV_OK) {
        UVM_ERR_PRINT("nvUvmInterfaceSetPageDirectory() failed: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        return status;
    }
    gpu->rm_address_space_moved_to_page_tree = true;

    return NV_OK;
}

static void deconfigure_address_space(uvm_gpu_t *gpu)
{
    if (gpu->rm_address_space_moved_to_page_tree)
        uvm_rm_locked_call_void(nvUvmInterfaceUnsetPageDirectory(gpu->rm_address_space));

    if (gpu->address_space_tree.root)
        uvm_page_tree_deinit(&gpu->address_space_tree);
}

static NV_STATUS init_big_pages(uvm_gpu_t *gpu)
{
    NV_STATUS status;

    if (!gpu->big_page.swizzling)
        return NV_OK;

    status = uvm_mmu_create_big_page_identity_mappings(gpu);
    if (status != NV_OK)
        return status;

    status = uvm_pmm_gpu_alloc_kernel(&gpu->pmm,
                                      1,
                                      gpu->big_page.internal_size,
                                      UVM_PMM_ALLOC_FLAGS_NONE,
                                      &gpu->big_page.staging.chunk,
                                      &gpu->big_page.staging.tracker);
    if (status != NV_OK)
        return status;

    return NV_OK;
}

static void deinit_big_pages(uvm_gpu_t *gpu)
{
    if (!gpu->big_page.swizzling)
        return;

    (void)uvm_tracker_wait_deinit(&gpu->big_page.staging.tracker);
    uvm_pmm_gpu_free(&gpu->pmm, gpu->big_page.staging.chunk, NULL);
    uvm_mmu_destroy_big_page_identity_mappings(gpu);
}

bool uvm_gpu_supports_replayable_faults(uvm_gpu_t *gpu)
{
    return uvm_hal_fault_buffer_class_supports_replayable_faults(gpu->fault_buffer_class);
}

bool uvm_gpu_supports_next_faults(uvm_gpu_t *gpu)
{
    return uvm_hal_fault_buffer_class_supports_next_faults(gpu->fault_buffer_class);
}

// Add a new gpu and register it with RM
static NV_STATUS add_gpu(NvProcessorUuid *gpu_uuid, uvm_gpu_t **gpu_out)
{
    NV_STATUS status;
    uvm_gpu_t *gpu;
    UvmGpuPlatformInfo gpu_platform_info = {0};

    uvm_assert_mutex_locked(&g_uvm_global.global_lock);

    status = uvm_rm_locked_call(nvUvmInterfaceRegisterGpu(gpu_uuid, &gpu_platform_info));
    if (status != NV_OK)
        return status;

    status = alloc_gpu(gpu_uuid, &gpu);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Failed to allocate a GPU object: %s\n", nvstatusToString(status));
        // Handle the clean up here as we didn't manage to get a uvm_gpu_t and cannot use remove_gpu()
        uvm_rm_locked_call_void(nvUvmInterfaceUnregisterGpu(gpu_uuid));
        return status;
    }

    // After this point all error clean up should be handled by remove_gpu()

    gpu->pci_dev = gpu_platform_info.pci_dev;
    gpu->dma_addressable_start = gpu_platform_info.dma_addressable_start;
    gpu->dma_addressable_limit = gpu_platform_info.dma_addressable_limit;

    status = get_gpu_info(gpu);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Failed to get GPU info: %s\n", nvstatusToString(status));
        goto error;
    }

    if (gpu->is_simulated)
        ++g_uvm_global.num_simulated_devices;

    if (!gpu_supports_uvm(gpu)) {
        UVM_DBG_PRINT("Register of non-UVM-capable GPU attempted: GPU %s\n", gpu->name);
        status = NV_ERR_NOT_SUPPORTED;
        goto error;
    }

    // Initialize the per-GPU procfs dirs as early as possible so that other
    // parts of the driver can add files in them as part of their per-GPU init.
    status = init_procfs_dirs(gpu);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Failed to init procfs dirs: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        goto error;
    }

    status = uvm_hal_init_gpu(gpu);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Failed to init GPU hal: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        goto error;
    }

    gpu->arch_hal->init_properties(gpu);
    uvm_mmu_init_gpu_peer_addresses(gpu);

    status = uvm_rm_locked_call(nvUvmInterfaceAddressSpaceCreate(g_uvm_global.rm_session_handle, &gpu->uuid,
            &gpu->rm_address_space, gpu->rm_va_base, gpu->rm_va_size));

    if (status != NV_OK) {
        UVM_ERR_PRINT("Creating RM address space failed: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        goto error;
    }

    status = get_gpu_caps(gpu);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Failed to get GPU caps: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        goto error;
    }

    status = uvm_gpu_check_ecc_error(gpu);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Initial ECC error check failed: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        goto error;
    }

    status = uvm_pmm_gpu_init(gpu, &gpu->pmm);
    if (status != NV_OK) {
        UVM_ERR_PRINT("PMM initialization failed: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        goto error;
    }

    status = init_semaphore_pool(gpu);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Failed to initialize the semaphore pool: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        goto error;
    }

    status = uvm_channel_manager_create(gpu, &gpu->channel_manager);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Failed to initialize the channel manager: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        goto error;
    }

    status = configure_address_space(gpu);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Failed to configure the GPU address space: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        goto error;
    }

    status = init_big_pages(gpu);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Failed to init big pages: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        goto error;
    }

    status = init_procfs_files(gpu);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Failed to init procfs files: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        goto error;
    }

    if (uvm_gpu_supports_replayable_faults(gpu)) {
        status = uvm_gpu_fault_buffer_init(gpu);
        if (status != NV_OK) {
            UVM_ERR_PRINT("Failed to initialize GPU fault buffer: %s, GPU: %s\n", nvstatusToString(status), gpu->name);
            goto error;
        }

        nv_kthread_q_item_init(&gpu->bottom_half_q_item, uvm8_isr_bottom_half, gpu);

        // This causes a (severely) truncated version of the gpu->name to show up as the
        // name of a kthread, as seen via the ps(1) utility. Example: [ID 1: GeForce G]
        status = nv_kthread_q_init(&gpu->bottom_half_q, gpu->name);
        if (status != NV_OK) {
            UVM_ERR_PRINT("Failed in nv_kthread_q_init_and_run: %s, GPU %s\n", nvstatusToString(status), gpu->name);
            goto error;
        }

        gpu->handling_replayable_faults = true;
    }

    // Handle any future chip or future release items:
    status = uvm_next_add_gpu(gpu);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Failed in uvm_next_add_gpu: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        goto error;
    }

    status = uvm_hmm_device_register(gpu);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Failed to register HMM device: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        goto error;
    }

    atomic64_set(&gpu->retained_count, 1);
    uvm_processor_mask_set(&g_uvm_global.retained_gpus, gpu->id);

    // Add the GPU to the GPU table.
    uvm_spin_lock_irqsave(&g_uvm_global.gpu_table_lock);

    // The gpu array is offset by 1 to accomodate the UVM_CPU_ID (0).
    g_uvm_global.gpus[(gpu->id) - 1] = gpu;

    // Although locking correctness does not, at this early point (before the GPU is visible in
    // the table) strictly require holding the gpu_table_lock in order to read
    // gpu->handling_replayable_faults, nor to enable page fault interrupts (this could have
    // been done earlier), it is best to do it here, in order to avoid an interrupt storm. That
    // way, we take advantage of the spinlock_irqsave side effect of turning off local CPU
    // interrupts, as part of holding the gpu_table_lock. That means that the local CPU won't
    // receive any of these interrupts, until the GPU is safely added to the table (where the
    // top half ISR can find it).
    //
    // As usual with spinlock_irqsave behavior, *other* CPUs can still handle these interrupts,
    // but the local CPU will not be slowed down (interrupted) by such handling, and can
    // quickly release the gpu_table_lock, thus unblocking any other CPU's top half (which
    // waits for the gpu_table_lock).
    if (gpu->handling_replayable_faults)
        gpu->fault_buffer_hal->enable_replayable_faults(gpu);

    uvm_spin_unlock_irqrestore(&g_uvm_global.gpu_table_lock);

    *gpu_out = gpu;

    return NV_OK;

error:
    remove_gpu(gpu);

    return status;
}

// Remove all references the given GPU has to other GPUs, since one of those
// other GPUs is getting removed. This involves waiting for any unfinished
// trackers contained by this GPU.
static void remove_gpus_from_gpu(uvm_gpu_t *gpu)
{
    NV_STATUS status;

    // Sync the replay tracker since it inherits dependencies from the VA block
    // trackers.
    if (gpu->handling_replayable_faults) {
        uvm_gpu_isr_lock(gpu);
        status = uvm_tracker_wait(&gpu->fault_buffer_info.replayable.replay_tracker);
        uvm_gpu_isr_unlock(gpu);

        if (status != NV_OK)
            UVM_ASSERT(status == uvm_global_get_status());
    }

    uvm_mutex_lock(&gpu->big_page.staging.lock);
    status = uvm_tracker_wait(&gpu->big_page.staging.tracker);
    uvm_mutex_unlock(&gpu->big_page.staging.lock);
    if (status != NV_OK)
        UVM_ASSERT(status == uvm_global_get_status());

    // Sync all trackers in PMM
    uvm_pmm_gpu_sync(&gpu->pmm);
}

// Remove a gpu and unregister it from RM
// Note that this is also used in most error paths in add_gpu()
static void remove_gpu(uvm_gpu_t *gpu)
{
    bool was_handling_replayable_faults;
    uvm_gpu_t *other_gpu;

    uvm_assert_mutex_locked(&g_uvm_global.global_lock);
    UVM_ASSERT_MSG(uvm_gpu_retained_count(gpu) == 0, "gpu_id %u retained_count %llu\n",
                   gpu->id, uvm_gpu_retained_count(gpu));

    // All channels should have been removed before the retained count went to 0
    UVM_ASSERT(radix_tree_empty(&gpu->instance_ptr_table));

    // Remove the GPU from the table.
    uvm_spin_lock_irqsave(&g_uvm_global.gpu_table_lock);

    // The gpu array is offset by 1 to accomodate the UVM_CPU_ID (0).
    g_uvm_global.gpus[(gpu->id) - 1] = NULL;
    uvm_spin_unlock_irqrestore(&g_uvm_global.gpu_table_lock);

    uvm_processor_mask_clear(&g_uvm_global.retained_gpus, gpu->id);

    // Now that the GPU is safely out of the global table, lock the GPU and mark
    // it as no longer handling interrupts so the top half knows not to schedule
    // any more bottom halves.
    uvm_spin_lock_irqsave(&gpu->page_fault_interrupts_lock);
    was_handling_replayable_faults = gpu->handling_replayable_faults;

    if (was_handling_replayable_faults)
        uvm_gpu_disable_replayable_faults(gpu);

    gpu->handling_replayable_faults = false;
    uvm_spin_unlock_irqrestore(&gpu->page_fault_interrupts_lock);

    // Flush all bottom half ISR work items and stop the nv_kthread_q that is
    // servicing this GPU's bottom half ISR. Note that this requires that the
    // bottom half never take the global lock, since we're holding it in write
    // mode here.
    if (was_handling_replayable_faults)
        nv_kthread_q_stop(&gpu->bottom_half_q);

    // Remove any pointers to this GPU from other GPUs' trackers.
    for_each_global_gpu(other_gpu) {
        UVM_ASSERT(other_gpu != gpu);
        remove_gpus_from_gpu(other_gpu);
    }

    uvm_hmm_device_unregister(gpu);

    // Handle any future chip or future release items:
    uvm_next_remove_gpu(gpu);

    // Return ownership to RM:
    if (was_handling_replayable_faults) {
        // No user threads could have anything left on disable_intr_ref_count
        // since they must retain the GPU across uvm_gpu_isr_lock/
        // uvm_gpu_isr_unlock. This means the uvm_gpu_disable_replayable_faults
        // above could only have raced with bottom halves.
        //
        // If we cleared handling_replayable_faults above before the bottom half
        // got to its uvm_gpu_isr_unlock, when it eventually reached
        // uvm_gpu_isr_unlock it would have skipped the disable, leaving us with
        // extra ref counts here.
        //
        // In any case we're guaranteed that replayable interrupts are disabled
        // and can't get re-enabled, so we can safely ignore the ref count value
        // and just clean things up.
        UVM_ASSERT_MSG(gpu->disable_intr_ref_count > 0, "%s disable_intr_ref_count: %llu\n",
                       gpu->name, gpu->disable_intr_ref_count);
        uvm_gpu_fault_buffer_deinit(gpu);
    }

    deinit_procfs_files(gpu);

    deinit_big_pages(gpu);

    // Wait for any deferred frees and their associated trackers to be finished
    // before tearing down channels.
    uvm_pmm_gpu_sync(&gpu->pmm);

    uvm_channel_manager_destroy(gpu->channel_manager);

    // Deconfigure the address space only after destroying all the channels as
    // in case any of them hit fatal errors, RM will assert that they are not
    // idle during nvUvmInterfaceUnsetPageDirectory() and that's an unnecessary
    // pain during development.
    deconfigure_address_space(gpu);

    deinit_semaphore_pool(gpu);

    uvm_pmm_gpu_deinit(&gpu->pmm);

    if (gpu->rm_address_space != 0)
        uvm_rm_locked_call_void(nvUvmInterfaceAddressSpaceDestroy(gpu->rm_address_space));

    // After calling nvUvmInterfaceUnregisterGpu() the reference to pci_dev may
    // not be valid any more so clear it ahead of time.
    gpu->pci_dev = NULL;
    uvm_rm_locked_call_void(nvUvmInterfaceUnregisterGpu(&gpu->uuid));

    deinit_procfs_dirs(gpu);

    if (gpu->is_simulated)
        --g_uvm_global.num_simulated_devices;

    uvm_gpu_kref_put(gpu);
}

// Do not not call this directly. It is called by kref_put, when the GPU's ref count drops
// to zero.
static void uvm_gpu_destroy(struct kref *kref)
{
    uvm_gpu_t *gpu = container_of(kref, uvm_gpu_t, gpu_kref);

    UVM_ASSERT_MSG(uvm_gpu_retained_count(gpu) == 0, "gpu_id %u retained_count %llu\n",
                   gpu->id, uvm_gpu_retained_count(gpu));

    gpu->magic = 0;

    uvm_kvfree(gpu);
}

void uvm_gpu_kref_put(uvm_gpu_t *gpu)
{
    kref_put(&gpu->gpu_kref, uvm_gpu_destroy);
}

void update_stats_fault_cb(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data)
{
    uvm_gpu_t *gpu;

    UVM_ASSERT(event_id == UVM_PERF_EVENT_FAULT);

    if (event_data->fault.proc_id == UVM_CPU_ID)
        return;

    gpu = uvm_gpu_get(event_data->fault.proc_id);
    switch (event_data->fault.gpu.buffer_entry->fault_access_type)
    {
        case UVM_FAULT_ACCESS_TYPE_PREFETCH:
            ++gpu->fault_buffer_info.replayable.stats.num_prefetch_faults;
            break;
        case UVM_FAULT_ACCESS_TYPE_READ:
            ++gpu->fault_buffer_info.replayable.stats.num_read_faults;
            break;
        case UVM_FAULT_ACCESS_TYPE_WRITE:
            ++gpu->fault_buffer_info.replayable.stats.num_write_faults;
            break;
        case UVM_FAULT_ACCESS_TYPE_ATOMIC:
            ++gpu->fault_buffer_info.replayable.stats.num_atomic_faults;
            break;
        default:
            break;
    }
    ++gpu->stats.num_faults;
}

void update_stats_migration_cb(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data)
{
    uvm_gpu_t *gpu_dst = NULL;
    uvm_gpu_t *gpu_src = NULL;
    NvU64 pages;

    UVM_ASSERT(event_id == UVM_PERF_EVENT_MIGRATION);

    if (event_data->migration.dst != UVM_CPU_ID)
        gpu_dst = uvm_gpu_get(event_data->migration.dst);

    if (event_data->migration.src != UVM_CPU_ID)
        gpu_src = uvm_gpu_get(event_data->migration.src);

    if (!gpu_dst && !gpu_src)
        return;

    pages = event_data->migration.bytes / PAGE_SIZE;
    UVM_ASSERT(event_data->migration.bytes % PAGE_SIZE == 0);
    UVM_ASSERT(pages > 0);

    if (gpu_dst) {
        // TODO: Bug 1716025: discard non-fault migrations for fault stats
        atomic64_add(pages, &gpu_dst->fault_buffer_info.replayable.stats.num_pages_in);
        atomic64_add(pages, &gpu_dst->stats.num_pages_in);
    }
    if (gpu_src) {
        // TODO: Bug 1716025: discard non-fault migrations for fault stats
        atomic64_add(pages, &gpu_src->fault_buffer_info.replayable.stats.num_pages_out);
        atomic64_add(pages, &gpu_src->stats.num_pages_out);
    }
}

NV_STATUS uvm_gpu_init(void)
{
    NV_STATUS status;
    status = uvm_hal_init_table();
    if (status != NV_OK) {
        UVM_ERR_PRINT("uvm_hal_init_table() failed: %s\n", nvstatusToString(status));
        return status;
    }

    return NV_OK;
}

void uvm_gpu_exit(void)
{
    uvm_gpu_t *gpu;
    uvm_gpu_id_t id;

    for_each_gpu_id(id) {
        gpu = uvm_gpu_get(id);
        UVM_ASSERT_MSG(gpu == NULL, "GPU still present: %s\n", gpu->name);
    }

    // CPU should never be in the retained GPUs mask
    UVM_ASSERT(!uvm_processor_mask_test(&g_uvm_global.retained_gpus, UVM_CPU_ID));

    uvm_hal_free_table();
}

NV_STATUS uvm_gpu_init_va_space(uvm_va_space_t *va_space)
{
    NV_STATUS status;

    if (uvm_procfs_is_debug_enabled()) {
        status = uvm_perf_register_event_callback(&va_space->perf_events,
                                                  UVM_PERF_EVENT_FAULT,
                                                  update_stats_fault_cb);
        if (status != NV_OK)
            return status;

        status = uvm_perf_register_event_callback(&va_space->perf_events,
                                                  UVM_PERF_EVENT_MIGRATION,
                                                  update_stats_migration_cb);
        if (status != NV_OK)
            return status;
    }

    return NV_OK;
}

uvm_gpu_t *uvm_gpu_get_by_uuid_locked(NvProcessorUuid *gpu_uuid)
{
    uvm_gpu_id_t id;

    for_each_gpu_id(id) {
        uvm_gpu_t *gpu = uvm_gpu_get(id);
        if (gpu && uvm_processor_uuid_eq(&gpu->uuid, gpu_uuid))
            return gpu;
    }

    return NULL;
}

uvm_gpu_t *uvm_gpu_get_by_uuid(NvProcessorUuid *gpu_uuid)
{
    uvm_assert_mutex_locked(&g_uvm_global.global_lock);

    return uvm_gpu_get_by_uuid_locked(gpu_uuid);
}

NV_STATUS uvm_gpu_retain_by_uuid_locked(NvProcessorUuid *gpu_uuid, uvm_gpu_t **gpu_out)
{
    NV_STATUS status = NV_OK;
    uvm_gpu_t *gpu;

    uvm_assert_mutex_locked(&g_uvm_global.global_lock);

    gpu = uvm_gpu_get_by_uuid(gpu_uuid);

    if (gpu == NULL)
        status = add_gpu(gpu_uuid, &gpu);
    else
        atomic64_inc(&gpu->retained_count);

    *gpu_out = gpu;

    return status;
}

NV_STATUS uvm_gpu_retain_by_uuid(NvProcessorUuid *gpu_uuid, uvm_gpu_t **gpu_out)
{
    NV_STATUS status;
    uvm_mutex_lock(&g_uvm_global.global_lock);
    status = uvm_gpu_retain_by_uuid_locked(gpu_uuid, gpu_out);
    uvm_mutex_unlock(&g_uvm_global.global_lock);
    return status;
}

void uvm_gpu_retain(uvm_gpu_t *gpu)
{
    UVM_ASSERT(uvm_gpu_retained_count(gpu) > 0);
    atomic64_inc(&gpu->retained_count);
}

void uvm_gpu_release_locked(uvm_gpu_t *gpu)
{
    uvm_assert_mutex_locked(&g_uvm_global.global_lock);
    UVM_ASSERT(uvm_gpu_retained_count(gpu) > 0);

    if (atomic64_dec_and_test(&gpu->retained_count))
        remove_gpu(gpu);
}

void uvm_gpu_release(uvm_gpu_t *gpu)
{
    uvm_mutex_lock(&g_uvm_global.global_lock);
    uvm_gpu_release_locked(gpu);
    uvm_mutex_unlock(&g_uvm_global.global_lock);
}

void uvm_gpu_retain_mask(const uvm_processor_mask_t *mask)
{
    uvm_gpu_t *gpu;
    for_each_gpu_in_mask(gpu, mask)
        uvm_gpu_retain(gpu);
}

void uvm_gpu_release_mask_locked(const uvm_processor_mask_t *mask)
{
    uvm_gpu_id_t gpu_id;
    // Do not use for_each_gpu_in_mask as it reads the GPU state and it might get destroyed
    for_each_gpu_id_in_mask(gpu_id, mask)
        uvm_gpu_release_locked(uvm_gpu_get(gpu_id));
}

void uvm_gpu_release_mask(const uvm_processor_mask_t *mask)
{
    uvm_mutex_lock(&g_uvm_global.global_lock);
    uvm_gpu_release_mask_locked(mask);
    uvm_mutex_unlock(&g_uvm_global.global_lock);
}

// Note: Peer table is an upper triangular matrix packed into a flat array.
// This function converts an index of 2D array of size [N x N] into an index
// of upper triangular array of size [((N - 1) * ((N - 1) + 1)) / 2] which does not
// include diagonal elements.
NvU32 uvm_gpu_peer_table_index(uvm_gpu_id_t gpu_id_1, uvm_gpu_id_t gpu_id_2)
{
    NvU32 square_index, triangular_index;

    UVM_ASSERT(gpu_id_1 != gpu_id_2);

    // Calculate an index of 2D array by re-ordering indices to always point to the same entry.
    square_index = ((min(gpu_id_1, gpu_id_2) - 1) * UVM8_MAX_GPUS) + (max(gpu_id_1, gpu_id_2) - 1);

    // Calculate and subtract number of lower triangular matrix elements till the current row
    // (which includes diagonal elements) to get the correct index in an upper triangular matrix.
    // Note: As gpu_id can be [1, N), no extra logic is needed to calculate diagonal elements.
    triangular_index = square_index - SUM_FROM_0_TO_N(min(gpu_id_1, gpu_id_2));

    UVM_ASSERT(triangular_index < UVM8_MAX_UNIQUE_GPU_PAIRS);

    return triangular_index;
}

static NV_STATUS service_interrupts(uvm_gpu_t *gpu)
{
    // Asking RM to service interrupts from top half interrupt handler would
    // very likely deadlock.
    UVM_ASSERT(!in_interrupt());

    return uvm_rm_locked_call(nvUvmInterfaceServiceDeviceInterruptsRM(gpu->rm_address_space));
}

NV_STATUS uvm_gpu_check_ecc_error_no_rm(uvm_gpu_t *gpu)
{
    // We may need to call service_interrupts() which cannot be done in the top
    // half interrupt handler so assert here as well to catch improper use as
    // early as possible.
    UVM_ASSERT(!in_interrupt());

    if (!gpu->ecc.enabled)
        return NV_OK;

    // Early out If a global ECC error is already set to not spam the logs with
    // the same error.
    if (uvm_global_get_status() == NV_ERR_ECC_ERROR)
        return NV_ERR_ECC_ERROR;

    if (*gpu->ecc.error_notifier) {
        UVM_ERR_PRINT("ECC error encountered, GPU %s\n", gpu->name);
        uvm_global_set_fatal_error(NV_ERR_ECC_ERROR);
        return NV_ERR_ECC_ERROR;
    }

    // RM hasn't seen an ECC error yet, check whether there is a pending
    // interrupt that might indicate one. We might get false positives because
    // the interrupt bits we read are not ECC-specific. They're just the
    // top-level bits for any interrupt on all engines which support ECC. On
    // Pascal for example, RM returns us a mask with the bits for GR, L2, and
    // FB, because any of those might raise an ECC interrupt. So if they're set
    // we have to ask RM to check whether it was really an ECC error (and a
    // double-bit ECC error at that), in which case it sets the notifier.
    if ((*gpu->ecc.hw_interrupt_tree_location & gpu->ecc.mask) == 0) {
        // No pending interrupts.
        return NV_OK;
    }

    // An interrupt that might mean an ECC error needs to be serviced, signal
    // that to the caller.
    return NV_WARN_MORE_PROCESSING_REQUIRED;
}

NV_STATUS uvm_gpu_check_ecc_error(uvm_gpu_t *gpu)
{
    NV_STATUS status = uvm_gpu_check_ecc_error_no_rm(gpu);

    if (status == NV_OK || status != NV_WARN_MORE_PROCESSING_REQUIRED)
        return status;

    // An interrupt that might mean an ECC error needs to be serviced.
    UVM_ASSERT(status == NV_WARN_MORE_PROCESSING_REQUIRED);

    status = service_interrupts(gpu);
    if (status != NV_OK) {
        UVM_ERR_PRINT("Servicing interrupts failed: %s, GPU %s\n", nvstatusToString(status), gpu->name);
        return status;
    }

    // After servicing interrupts the ECC error notifier should be current.
    if (*gpu->ecc.error_notifier) {
        UVM_ERR_PRINT("ECC error encountered, GPU %s\n", gpu->name);
        uvm_global_set_fatal_error(NV_ERR_ECC_ERROR);
        return NV_ERR_ECC_ERROR;
    }

    return NV_OK;
}

NV_STATUS uvm_gpu_check_ecc_error_mask(uvm_processor_mask_t *gpus)
{
    uvm_gpu_t *gpu;
    for_each_gpu_in_mask(gpu, gpus) {
        NV_STATUS status = uvm_gpu_check_ecc_error(gpu);
        if (status != NV_OK)
            return status;
    }

    return NV_OK;
}

static NV_STATUS enable_peer_access(uvm_gpu_t *gpu_1, uvm_gpu_t *gpu_2)
{
    NV_STATUS status = NV_OK;
    UvmGpuP2PCapsParams p2p_caps_params;
    uvm_gpu_peer_t *peer_caps;
    NvHandle p2p_handle = 0;

    UVM_ASSERT(gpu_1);
    UVM_ASSERT(gpu_2);
    uvm_assert_mutex_locked(&g_uvm_global.global_lock);

    status = uvm_rm_locked_call(nvUvmInterfaceP2pObjectCreate(g_uvm_global.rm_session_handle,
                                                              &gpu_1->uuid,
                                                              &gpu_2->uuid,
                                                              &p2p_handle));
    if (status != NV_OK) {
        UVM_DBG_PRINT("enable_peer_access failed to create a P2P object with error: %s, for GPU1:%s and GPU2:%s \n",
                       nvstatusToString(status), gpu_1->name, gpu_2->name);
        return status;
    }

    // Store the handle in the global table.
    peer_caps = uvm_gpu_peer_caps(gpu_1, gpu_2);
    peer_caps->p2p_handle = p2p_handle;

    memset(&p2p_caps_params, 0, sizeof(UvmGpuP2PCapsParams));

    p2p_caps_params.pUuids[0] = gpu_1->id < gpu_2->id ? gpu_1->uuid.uuid : gpu_2->uuid.uuid;
    p2p_caps_params.pUuids[1] = gpu_1->id > gpu_2->id ? gpu_1->uuid.uuid : gpu_2->uuid.uuid;

    status = uvm_rm_locked_call(nvUvmInterfaceGetP2PCaps(&p2p_caps_params));
    if (status != NV_OK) {
        UVM_ERR_PRINT("enable_peer_access failed to query P2P caps with error: %s, for GPU1:%s and GPU2:%s \n",
                       nvstatusToString(status), gpu_1->name, gpu_2->name);
        goto cleanup;
    }

    // check for peer-to-peer compatibility (PCI-E or NvLink).
    if (p2p_caps_params.propSupported) {
        peer_caps->link_type = UVM_GPU_LINK_PCIE;
    }
    else if (p2p_caps_params.nvlinkSupported) {
        peer_caps->link_type = UVM_GPU_LINK_NVLINK_1;
    }
    else {
        status = NV_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    peer_caps->atomic_supported = p2p_caps_params.atomicSupported;

    // Peer id from min(gpu_id_1, gpu_id_2) -> max(gpu_id_1, gpu_id_2)
    peer_caps->peer_ids[0] = p2p_caps_params.peerIds[0];

    // Peer id from max(gpu_id_1, gpu_id_2) -> min(gpu_id_1, gpu_id_2)
    peer_caps->peer_ids[1] = p2p_caps_params.peerIds[1];

    // establish peer mappings from each GPU to the other
    status = uvm_mmu_create_peer_identity_mappings(gpu_1, gpu_2);
    if (status != NV_OK)
        goto cleanup;

    status = uvm_mmu_create_peer_identity_mappings(gpu_2, gpu_1);
    if (status != NV_OK)
        goto cleanup;

    return NV_OK;

cleanup:
    disable_peer_access(gpu_1, gpu_2);
    return status;
}

NV_STATUS uvm_gpu_retain_peer_access(uvm_gpu_t *gpu_1, uvm_gpu_t *gpu_2)
{
    NV_STATUS status = NV_OK;
    uvm_gpu_peer_t *peer_caps;

    UVM_ASSERT(gpu_1);
    UVM_ASSERT(gpu_2);
    uvm_assert_mutex_locked(&g_uvm_global.global_lock);

    peer_caps = uvm_gpu_peer_caps(gpu_1, gpu_2);

    // Insert an entry into global peer table, if not present.
    if (peer_caps->registered_ref_count == 0) {
        status = enable_peer_access(gpu_1, gpu_2);
        if (status != NV_OK)
            return status;
    }

    // GPUs can't be destroyed until their peer pairings have also been
    // destroyed.
    uvm_gpu_retain(gpu_1);
    uvm_gpu_retain(gpu_2);

    peer_caps->registered_ref_count++;

    return status;
}

static void disable_peer_access(uvm_gpu_t *gpu_1, uvm_gpu_t *gpu_2)
{
    uvm_gpu_peer_t *peer_caps;
    NvHandle p2p_handle = 0;

    UVM_ASSERT(gpu_1);
    UVM_ASSERT(gpu_2);

    uvm_assert_mutex_locked(&g_uvm_global.global_lock);

    peer_caps = uvm_gpu_peer_caps(gpu_1, gpu_2);
    p2p_handle = peer_caps->p2p_handle;

    UVM_ASSERT(p2p_handle);

    uvm_mmu_destroy_peer_identity_mappings(gpu_1, gpu_2);
    uvm_mmu_destroy_peer_identity_mappings(gpu_2, gpu_1);

    uvm_rm_locked_call_void(nvUvmInterfaceP2pObjectDestroy(g_uvm_global.rm_session_handle, p2p_handle));

    memset(peer_caps, 0, sizeof(uvm_gpu_peer_t));
}

void uvm_gpu_release_peer_access(uvm_gpu_t *gpu_1, uvm_gpu_t *gpu_2)
{
    uvm_gpu_peer_t *p2p_caps;
    UVM_ASSERT(gpu_1);
    UVM_ASSERT(gpu_2);
    uvm_assert_mutex_locked(&g_uvm_global.global_lock);

    p2p_caps = uvm_gpu_peer_caps(gpu_1, gpu_2);

    UVM_ASSERT(p2p_caps->registered_ref_count > 0);
    p2p_caps->registered_ref_count--;

    if (p2p_caps->registered_ref_count == 0)
        disable_peer_access(gpu_1, gpu_2);

    uvm_gpu_release_locked(gpu_1);
    uvm_gpu_release_locked(gpu_2);
}

uvm_aperture_t uvm_gpu_peer_aperture(uvm_gpu_t *local_gpu, uvm_gpu_t *remote_gpu)
{
    size_t peer_index;
    uvm_gpu_peer_t *p2p_caps;

    UVM_ASSERT(local_gpu != remote_gpu);

    p2p_caps = uvm_gpu_peer_caps(local_gpu, remote_gpu);

    if (local_gpu->id < remote_gpu->id)
        peer_index = 0;
    else
        peer_index = 1;

    return UVM_APERTURE_PEER(p2p_caps->peer_ids[peer_index]);
}

uvm_gpu_peer_t *uvm_gpu_index_peer_caps(uvm_gpu_id_t gpu_id1, uvm_gpu_id_t gpu_id2)
{
    NvU32 table_index = uvm_gpu_peer_table_index(gpu_id1, gpu_id2);
    return &g_uvm_global.peers[table_index];
}

uvm_gpu_address_t uvm_gpu_peer_memory_address(uvm_gpu_t *local_gpu, uvm_gpu_t *peer_gpu, uvm_gpu_phys_address_t addr)
{
    NvU32 peer_id = UVM_APERTURE_PEER_ID(uvm_gpu_peer_aperture(local_gpu, peer_gpu));
    UVM_ASSERT(local_gpu->peer_identity_mappings_supported);
    UVM_ASSERT(addr.aperture == UVM_APERTURE_VID || addr.aperture == uvm_gpu_peer_aperture(local_gpu, peer_gpu));
    return uvm_gpu_address_virtual(local_gpu->peer_mappings[peer_id].base + addr.address);
}

static unsigned long instance_ptr_to_key(uvm_gpu_phys_address_t instance_ptr)
{
    NvU64 key;
    int is_sys = (instance_ptr.aperture == UVM_APERTURE_SYS);

    // Instance pointers must be 4k aligned and they must have either VID or SYS
    // apertures. Compress them as much as we can both to guarantee that the key
    // fits within 64 bits, and to make the table as shallow as possible.
    UVM_ASSERT(IS_ALIGNED(instance_ptr.address, UVM_PAGE_SIZE_4K));
    UVM_ASSERT(instance_ptr.aperture == UVM_APERTURE_VID || instance_ptr.aperture == UVM_APERTURE_SYS);

    key = (instance_ptr.address >> 11) | is_sys;
    UVM_ASSERT((unsigned long)key == key);

    return key;
}

NV_STATUS uvm_gpu_add_instance_ptr(uvm_gpu_t *gpu, uvm_gpu_phys_address_t instance_ptr, uvm_va_space_t *va_space)
{
    unsigned long key = instance_ptr_to_key(instance_ptr);
    int ret;

    // Pre-load the tree to allocate memory outside of the table lock. This
    // returns with preemption disabled.
    ret = radix_tree_preload(NV_UVM_GFP_FLAGS);
    if (ret != 0)
        return errno_to_nv_status(ret);

    uvm_spin_lock(&gpu->instance_ptr_table_lock);
    ret = radix_tree_insert(&gpu->instance_ptr_table, key, va_space);
    uvm_spin_unlock(&gpu->instance_ptr_table_lock);

    // This re-enables preemption
    radix_tree_preload_end();

    // Since we did the pre-load, and we shouldn't be adding duplicate entries,
    // this shouldn't fail.
    UVM_ASSERT_MSG(ret == 0, "Insert failed: %d\n", ret);

    return NV_OK;
}

uvm_va_space_t *uvm_gpu_instance_ptr_to_va_space(uvm_gpu_t *gpu, uvm_gpu_phys_address_t instance_ptr)
{
    unsigned long key = instance_ptr_to_key(instance_ptr);
    uvm_va_space_t *va_space;
    uvm_spin_lock(&gpu->instance_ptr_table_lock);
    va_space = (uvm_va_space_t *)radix_tree_lookup(&gpu->instance_ptr_table, key);
    uvm_spin_unlock(&gpu->instance_ptr_table_lock);
    return va_space;
}

void uvm_gpu_remove_instance_ptr(uvm_gpu_t *gpu, uvm_gpu_phys_address_t instance_ptr)
{
    unsigned long key = instance_ptr_to_key(instance_ptr);
    uvm_va_space_t *va_space;

    uvm_spin_lock(&gpu->instance_ptr_table_lock);
    va_space = (uvm_va_space_t *)radix_tree_delete(&gpu->instance_ptr_table, key);
    uvm_spin_unlock(&gpu->instance_ptr_table_lock);

    if (va_space)
        uvm_assert_rwsem_locked_write(&va_space->lock);
}

NV_STATUS uvm_gpu_swizzle_phys(uvm_gpu_t *gpu,
                               NvU64 big_page_phys_address,
                               uvm_gpu_swizzle_op_t op,
                               uvm_tracker_t *tracker)
{
    uvm_gpu_address_t staging_addr, phys_addr, identity_addr;
    uvm_push_t push;
    NV_STATUS status = NV_OK;

    UVM_ASSERT(gpu->big_page.swizzling);
    UVM_ASSERT(IS_ALIGNED(big_page_phys_address, gpu->big_page.internal_size));

    uvm_mutex_lock(&gpu->big_page.staging.lock);

    status = uvm_push_begin_acquire(gpu->channel_manager,
                                    UVM_CHANNEL_TYPE_GPU_INTERNAL,
                                    &gpu->big_page.staging.tracker,
                                    &push,
                                    "%s phys 0x%llx",
                                    op == UVM_GPU_SWIZZLE_OP_SWIZZLE ? "Swizzling" : "Deswizzling",
                                    big_page_phys_address);
    if (status != NV_OK)
        goto out;

    uvm_push_acquire_tracker(&push, tracker);

    staging_addr  = uvm_gpu_address_physical(UVM_APERTURE_VID, gpu->big_page.staging.chunk->address);
    phys_addr     = uvm_gpu_address_physical(UVM_APERTURE_VID, big_page_phys_address);
    identity_addr = uvm_mmu_gpu_address_for_big_page_physical(phys_addr, gpu);

    // Note that these copies are dependent so they must not be pipelined. We
    // need the default MEMBAR_SYS in case we're going to map a peer GPU to the
    // newly-swizzled memory later.
    if (op == UVM_GPU_SWIZZLE_OP_SWIZZLE) {
        gpu->ce_hal->memcopy(&push, staging_addr, phys_addr, gpu->big_page.internal_size);
        gpu->ce_hal->memcopy(&push, identity_addr, staging_addr, gpu->big_page.internal_size);
    }
    else {
        gpu->ce_hal->memcopy(&push, staging_addr, identity_addr, gpu->big_page.internal_size);
        gpu->ce_hal->memcopy(&push, phys_addr, staging_addr, gpu->big_page.internal_size);
    }

    uvm_push_end(&push);

    uvm_tracker_overwrite_with_push(&gpu->big_page.staging.tracker, &push);

    if (tracker)
        uvm_tracker_overwrite_with_push(tracker, &push);

out:
    uvm_mutex_unlock(&gpu->big_page.staging.lock);
    return status;
}

void uvm_processor_uuid_from_id(NvProcessorUuid *uuid, uvm_processor_id_t id)
{
    if (id == UVM_CPU_ID) {
        memcpy(uuid, &NV_PROCESSOR_UUID_CPU_DEFAULT, sizeof(*uuid));
    }
    else {
        uvm_gpu_t *gpu = uvm_gpu_get(id);
        UVM_ASSERT(gpu);
        memcpy(uuid, &gpu->uuid, sizeof(*uuid));
    }
}

// This function implements the UvmRegisterGpu API call, as described in uvm.h. Notes:
//
// 1. The UVM VA space has a 1-to-1 relationship with an open instance of /dev/nvidia-uvm. That, in turn, has a 1-to-1
// relationship with a process, because the user-level UVM code (os-user-linux.c, for example) enforces an "open
// /dev/nvidia-uvm only once per process" policy. So a UVM VA space is very close to a process's VA space.
//
// If that user space code fails or is not used, then the relationship is no longer 1-to-1. That situation requires that
// this code should avoid crashing, leaking resources, exhibiting security holes, etc, but it does not have to provide
// correct UVM API behavior. Correct UVM API behavior requires doing the right things in user space before calling into
// the kernel.
//
// 2. The uvm_api*() routines are invoked directly from the top-level ioctl handler. They are considered "API routing
// routines", because they are responsible for providing the behavior that is described in the UVM user-to-kernel API
// documentation, in uvm.h.
//
// 3. A GPU VA space, which you'll see in other parts of the driver, is something different: there may be more than one
// GPU VA space within a process, and therefore within a UVM VA space.
//
NV_STATUS uvm_api_register_gpu(UVM_REGISTER_GPU_PARAMS *params, struct file *filp)

{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);

    return uvm_va_space_register_gpu(va_space, &params->gpu_uuid);
}

NV_STATUS uvm_api_unregister_gpu(UVM_UNREGISTER_GPU_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);

    return uvm_va_space_unregister_gpu(va_space, &params->gpu_uuid);
}

NV_STATUS uvm_api_enable_peer_access(UVM_ENABLE_PEER_ACCESS_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);

    return uvm_va_space_enable_peer_access(va_space, &params->gpuUuidA, &params->gpuUuidB);
}

NV_STATUS uvm_api_disable_peer_access(UVM_DISABLE_PEER_ACCESS_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);

    return uvm_va_space_disable_peer_access(va_space, &params->gpuUuidA, &params->gpuUuidB);
}

NV_STATUS uvm_api_register_gpu_va_space(UVM_REGISTER_GPU_VASPACE_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);
    uvm_rm_user_object_t user_rm_va_space =
    {
        .rm_control_fd = params->rmCtrlFd,
        .user_client   = params->hClient,
        .user_object   = params->hVaSpace
    };
    return uvm_va_space_register_gpu_va_space(va_space, &user_rm_va_space, &params->gpuUuid);
}

NV_STATUS uvm_api_unregister_gpu_va_space(UVM_UNREGISTER_GPU_VASPACE_PARAMS *params, struct file *filp)
{
    uvm_va_space_t *va_space = uvm_va_space_get(filp);
    return uvm_va_space_unregister_gpu_va_space(va_space, &params->gpuUuid);
}

NV_STATUS uvm8_test_set_prefetch_filtering(UVM_TEST_SET_PREFETCH_FILTERING_PARAMS *params, struct file *filp)
{
    uvm_gpu_t *gpu = NULL;
    NV_STATUS status = NV_OK;

    uvm_mutex_lock(&g_uvm_global.global_lock);

    gpu = uvm_gpu_get_by_uuid(&params->gpu_uuid);
    if (!gpu) {
        status = NV_ERR_INVALID_DEVICE;
        goto done;
    }

    switch (params->filtering_mode) {
        case UVM_TEST_PREFETCH_FILTERING_MODE_FILTER_ALL:
            gpu->arch_hal->disable_prefetch_faults(gpu);
            break;
        case UVM_TEST_PREFETCH_FILTERING_MODE_FILTER_NONE:
            gpu->arch_hal->enable_prefetch_faults(gpu);
            break;
        default:
            status = NV_ERR_INVALID_ARGUMENT;
            break;
    }

done:
    uvm_mutex_unlock(&g_uvm_global.global_lock);
    return status;
}
