/*******************************************************************************
    Copyright (c) 2013 NVIDIA Corporation

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

/*
 * This file contains code that is common to all variants of the (Linux) UVM
 * kernel module.
 *
 * Initially, UVM-Lite and Uvm-Next work is proceeding somewhat
 * independently, but the long- and short-term plan is for them to share the
 * same kernel module (this one). That's why, at least initially, you'll find
 * files for UVM-Lite, for Uvm-Next, and for common code. We'll refactor as
 * necessary, to achieve the right balance of sharing code and decoupling the
 * features.
 *
 */

#include "uvm_common.h"
#include "uvm_linux.h"

// TODO: Bug 1766109: Remove this when the GPU event stubs are no longer needed
#include "nv_uvm_interface.h"
#include "uvm_lite.h"

#include "uvm_channel_mgmt.h"

#include "uvm8_init.h"
#include "uvm8_forward_decl.h"

// TODO: Bug 1710855: Tweak this number through benchmarks
#define UVM_SPIN_LOOP_SCHEDULE_TIMEOUT_NS   (10*1000ULL)
#define UVM_SPIN_LOOP_PRINT_TIMEOUT_SEC     30ULL

static dev_t g_uvmBaseDev;
struct UvmOpsUvmEvents g_exportedUvmOps;

static char* uvm_driver_mode = "8";

// UVM-Lite behavior is activated through insmod argument: (default is "8")
//      insmod nvidia-uvm.ko uvm_driver_mode="lite"
//
// Similarly, the UVM-8 production driver can be activated with:
//      insmod nvidia-uvm.ko uvm_driver_mode="8"
module_param(uvm_driver_mode, charp, S_IRUGO);
MODULE_PARM_DESC(uvm_driver_mode,
                "Set the uvm kernel driver mode. Choices include: 8, lite.");

// Default to debug prints being enabled for debug and develop builds and
// disabled for release builds.
static int uvm_debug_prints = UVM_IS_DEBUG() || UVM_IS_DEVELOP();

// Make the module param writable so that prints can be enabled or disabled at
// any time by modifying the module parameter.
module_param(uvm_debug_prints, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(uvm_debug_prints, "Enable uvm debug prints.");

bool uvm_debug_prints_enabled()
{
    return uvm_debug_prints != 0;
}

typedef enum
{
    UVM_DRIVER_MODE_LITE,
    UVM_DRIVER_MODE_8,
} UvmDriverMode;

static const char * uvm_driver_mode_to_string(UvmDriverMode uvmDriverMode)
{
    switch (uvmDriverMode)
    {
        case UVM_DRIVER_MODE_LITE:
            return "lite";

        case UVM_DRIVER_MODE_8:
            return "8";
    }
    return "invalid";
}

static UvmDriverMode uvm_get_mode(void)
{
    static NvBool bUvmDriverModeChecked = NV_FALSE;
    static UvmDriverMode uvmDriverMode;

    if (!bUvmDriverModeChecked)
    {
        if (strcmp("lite", uvm_driver_mode) == 0)
            uvmDriverMode = UVM_DRIVER_MODE_LITE;
        else
            uvmDriverMode = UVM_DRIVER_MODE_8;

        bUvmDriverModeChecked = NV_TRUE;
    }

    return uvmDriverMode;
}

static NvBool uvm8_activated(void)
{
    return uvm_get_mode() == UVM_DRIVER_MODE_8;
}

static NvBool uvmlite_activated(void)
{
    return uvm_get_mode() == UVM_DRIVER_MODE_LITE;
}

NV_STATUS uvm_api_initialize(UVM_INITIALIZE_PARAMS *params, struct file *filp)
{
    if (uvm8_activated())
        params->rmStatus = uvm8_initialize(params, filp);
    else
        params->rmStatus = NV_OK;
    return params->rmStatus;
}

// This function serves to 'stub' out functionality by being a No-Op and
// returning NV_OK early.
NV_STATUS uvm_api_stub(void *pParams, struct file *filp)
{
    return NV_OK;
}

// This function serves to identify functionality that isn't supported
// in this UVM driver, by returning NV_ERR_NOT_SUPPORTED early.
NV_STATUS uvm_api_unsupported(void *pParams, struct file *filp)
{
    return NV_ERR_NOT_SUPPORTED;
}

//
// TODO: Bug 1766109: uvm8: delete UVM-Lite files and remove -lite mode
//  ...just remove -lite mode, instead of the original to-do: which was:
//
// This would be easier if RM allowed for multiple registrations, since we
//       could register UVM-Lite and UVM-Full separately (bug 1372835).
static NV_STATUS uvm_gpu_event_start_device(NvProcessorUuid *gpuUuidStruct)
{
    NV_STATUS nvStatus = NV_OK;

    if (uvmlite_activated())
        nvStatus = uvmlite_gpu_event_start_device(gpuUuidStruct);

    return nvStatus;
}

static NV_STATUS uvm_gpu_event_stop_device(NvProcessorUuid *gpuUuidStruct)
{
    NV_STATUS nvStatus = NV_OK;

    if (uvmlite_activated())
        nvStatus = uvmlite_gpu_event_stop_device(gpuUuidStruct);

    return nvStatus;
}

static NV_STATUS uvmSetupGpuProvider(void)
{
    NV_STATUS status = NV_OK;

    g_exportedUvmOps.startDevice = uvm_gpu_event_start_device;
    g_exportedUvmOps.stopDevice  = uvm_gpu_event_stop_device;

    // call RM to exchange the function pointers.
    status = nvUvmInterfaceRegisterUvmCallbacks(&g_exportedUvmOps);
    return status;
}

static int uvm_not8_init(dev_t uvmBaseDev)
{
    int ret = 0;
    int uvmliteInitialized = 0;
    int registered = 0;
    int channelApiInitialized = 0;
    NV_STATUS status;

    status = uvm_initialize_channel_mgmt_api();
    if (NV_OK != status)
    {
        UVM_ERR_PRINT_NV_STATUS("Could not initialize channel mgmt api.\n",
                                status);
        ret = -nv_status_to_errno(status);
        goto error;
    }
    channelApiInitialized = 1;

    ret = uvmlite_init(uvmBaseDev);
    if (ret)
        goto error;
    uvmliteInitialized = 1;

    // Register with RM
    //
    // TODO: Bug 1766109: uvm8: delete UVM-Lite files and remove -lite mode
    //  ...implement the above bugs, instead of doing the original to-do,
    // which was:
    //
    // This should probably happen before calling uvmlite_init and
    //       uvmfull_init, so those routines can make RM calls as part of their
    //       setup. But doing that means we could get event callbacks before
    //       those init routines are called, which is scary. Ideally this could
    //       be solved by having RM handle multiple registrations (bug 1372835).
    status = uvmSetupGpuProvider();
    if (NV_OK != status)
    {
        UVM_ERR_PRINT("Cannot register ops with NVIDIA driver. UVM error: 0x%x\n",
                        status);
        ret = -nv_status_to_errno(status);
        goto error;
    }
    registered = 1;

    // Query GPU list from RM
    //
    // TODO: Bug 1766109: uvm8: delete UVM-Lite files and remove -lite mode
    //  ...implement the above bugs, instead of doing the original to-do,
    // which was:
    //
    // This should be called within uvmlite_init but the RM calls aren't
    //       enabled until after the uvmSetupGpuProvider call above. This can be
    //       moved once multiple RM registrations are allowed (bug 1372835).
    //       Until then we need a dummy symbol.
    if(uvmliteInitialized && registered)
    {
        ret = uvmlite_setup_gpu_list();
        if (ret)
        {
            UVM_ERR_PRINT("uvm_lite_setup_gpu_list failed\n");
            goto error;
        }
    }

    return 0;

error:
    if (registered)
        nvUvmInterfaceDeRegisterUvmOps();

    if (channelApiInitialized)
        uvm_deinitialize_channel_mgmt_api();

    if (uvmliteInitialized)
        uvmlite_exit();

    return ret;
}

static int __init uvm_init(void)
{
    NvBool allocated_dev = NV_FALSE;

    // The various helper init routines will create their own minor devices, so
    // we only need to create space for them here.
    int ret = alloc_chrdev_region(&g_uvmBaseDev,
                              0,
                              NVIDIA_UVM_NUM_MINOR_DEVICES,
                              NVIDIA_UVM_DEVICE_NAME);
    if (ret != 0) {
        UVM_ERR_PRINT("alloc_chrdev_region failed: %d\n", ret);
        goto error;
    }
    allocated_dev = NV_TRUE;

    if (uvm8_activated())
        ret = uvm8_init(g_uvmBaseDev);
    else
        ret = uvm_not8_init(g_uvmBaseDev);

    if (ret != 0) {
        UVM_ERR_PRINT("uvm init failed: %d\n", ret);
        goto error;
    }

    pr_info("Loaded the UVM driver in %s mode, major device number %d\n",
            uvm_driver_mode_to_string(uvm_get_mode()), MAJOR(g_uvmBaseDev));

    if (uvm_enable_builtin_tests)
        pr_info("Built-in UVM tests are enabled. This is a security risk.\n");

    return 0;

error:
    if (allocated_dev)
        unregister_chrdev_region(g_uvmBaseDev, NVIDIA_UVM_NUM_MINOR_DEVICES);

    return ret;
}

static void uvm_not8_exit(void)
{
    // this will cleanup registered interfaces
    nvUvmInterfaceDeRegisterUvmOps();

    uvm_deinitialize_channel_mgmt_api();

    uvmlite_exit();
}

static void __exit uvm_exit(void)
{
    if (uvm8_activated())
        uvm8_exit();
    else
        uvm_not8_exit();

    unregister_chrdev_region(g_uvmBaseDev, NVIDIA_UVM_NUM_MINOR_DEVICES);

    pr_info("Unloaded the UVM driver in %s mode\n", uvm_driver_mode_to_string(uvm_get_mode()));
}

//
// Convert kernel errno codes to corresponding NV_STATUS
//
NV_STATUS errno_to_nv_status(int errnoCode)
{
    if (errnoCode < 0)
        errnoCode = -errnoCode;

    switch (errnoCode)
    {
        case 0:
            return NV_OK;

        case E2BIG:
        case EINVAL:
            return NV_ERR_INVALID_ARGUMENT;

        case EACCES:
            return NV_ERR_INVALID_ACCESS_TYPE;

        case EADDRINUSE:
        case EADDRNOTAVAIL:
            return NV_ERR_UVM_ADDRESS_IN_USE;

        case EFAULT:
            return NV_ERR_INVALID_ADDRESS;

        case EINTR:
        case EBUSY:
            return NV_ERR_BUSY_RETRY;

        case ENXIO:
        case ENODEV:
            return NV_ERR_MODULE_LOAD_FAILED;

        case ENOMEM:
            return NV_ERR_NO_MEMORY;

        case EPERM:
            return NV_ERR_INSUFFICIENT_PERMISSIONS;

        case ESRCH:
            return NV_ERR_PID_NOT_FOUND;

        case ETIMEDOUT:
            return NV_ERR_TIMEOUT;

        case EEXIST:
            return NV_ERR_IN_USE;

        case ENOSYS:
            return NV_ERR_NOT_SUPPORTED;

        case ENOENT:
            return NV_ERR_NO_VALID_PATH;

        case EIO:
            return NV_ERR_RC_ERROR;

        default:
            return NV_ERR_GENERIC;
    };
}

// Returns POSITIVE errno
int nv_status_to_errno(NV_STATUS status)
{
    switch (status) {
        case NV_OK:
            return 0;

        case NV_ERR_BUSY_RETRY:
            return EBUSY;

        case NV_ERR_INSUFFICIENT_PERMISSIONS:
            return EPERM;

        case NV_ERR_GPU_UUID_NOT_FOUND:
            return ENODEV;

        case NV_ERR_INSUFFICIENT_RESOURCES:
        case NV_ERR_NO_MEMORY:
            return ENOMEM;

        case NV_ERR_INVALID_ACCESS_TYPE:
            return EACCES;

        case NV_ERR_INVALID_ADDRESS:
            return EFAULT;

        case NV_ERR_INVALID_ARGUMENT:
        case NV_ERR_INVALID_DEVICE:
        case NV_ERR_INVALID_PARAMETER:
        case NV_ERR_INVALID_REQUEST:
        case NV_ERR_INVALID_STATE:
            return EINVAL;

        case NV_ERR_NOT_SUPPORTED:
            return ENOSYS;

        case NV_ERR_MODULE_LOAD_FAILED:
            return ENXIO;

        case NV_ERR_OVERLAPPING_UVM_COMMIT:
        case NV_ERR_UVM_ADDRESS_IN_USE:
            return EADDRINUSE;

        case NV_ERR_PID_NOT_FOUND:
            return ESRCH;

        case NV_ERR_TIMEOUT:
        case NV_ERR_TIMEOUT_RETRY:
            return ETIMEDOUT;

        case NV_ERR_IN_USE:
            return EEXIST;

        case NV_ERR_NO_VALID_PATH:
            return ENOENT;

        case NV_ERR_RC_ERROR:
        case NV_ERR_ECC_ERROR:
            return EIO;

        default:
            UVM_ASSERT_MSG(0, "No errno conversion set up for NV_STATUS %s\n", nvstatusToString(status));
            return EINVAL;
    }
}

//
// This routine retrieves the process ID of current, but makes no attempt to
// refcount or lock the pid in place, because that capability is only available
// to GPL-licenses device drivers.
//
// TODO: Bug 1483843: Use the GPL-protected routines if and when we are able to
// change over to a dual MIT/GPL license.
//
unsigned uvm_get_stale_process_id(void)
{
    return (unsigned) current->tgid;
}

unsigned uvm_get_stale_thread_id(void)
{
    return (unsigned) current->pid;
}

//
// A simple security rule for allowing access to UVM user space memory: if you
// are the same user as the owner of the memory, or if you are root, then you
// are granted access. The idea is to allow debuggers and profilers to work, but
// without opening up any security holes.
//
NvBool uvm_user_id_security_check(uid_t euidTarget)
{
    return (NV_CURRENT_EUID() == euidTarget) ||
           (UVM_ROOT_UID == euidTarget);
}

// Locking: you must hold mm->mmap_sem write lock before calling this function
NV_STATUS uvm_map_page(struct vm_area_struct *vma, struct page *page,
                       unsigned long addr)
{
    if (vm_insert_page(vma, addr, page) == 0)
        return NV_OK;

    return NV_ERR_INVALID_ARGUMENT;
}

void on_uvm_assert(void)
{
    (void)NULL;
}

NV_STATUS uvm_spin_loop(uvm_spin_loop_t *spin)
{
    NvU64 curr = NV_GETTIME();

    // This schedule() is required for functionality, not just system
    // performance. It allows RM to run and unblock the UVM driver:
    //
    // - UVM must service faults in order for RM to idle/preempt a context
    // - RM must service interrupts which stall UVM (SW methods, stalling CE
    //   interrupts, etc) in order for UVM to service faults
    //
    // Even though UVM's bottom half is preemptable, we have encountered cases
    // in which a user thread running in RM won't preempt the UVM driver's
    // thread unless the UVM driver thread gives up its timeslice. This is also
    // theoretically possible if the RM thread has a low nice priority.
    //
    // TODO: Bug 1710855: Look into proper prioritization of these threads as a longer-term
    //       solution.
    if (curr - spin->start_time_ns >= UVM_SPIN_LOOP_SCHEDULE_TIMEOUT_NS && NV_MAY_SLEEP()) {
        schedule();
        curr = NV_GETTIME();
    }

    cpu_relax();

    // TODO: Bug 1710855: Also check fatal_signal_pending() here if the caller can handle it.

    if (curr - spin->print_time_ns >= 1000*1000*1000*UVM_SPIN_LOOP_PRINT_TIMEOUT_SEC) {
        spin->print_time_ns = curr;
        return NV_ERR_TIMEOUT_RETRY;
    }

    return NV_OK;
}

module_init(uvm_init);
module_exit(uvm_exit);

// This parameter allows a program in user mode to call the kernel tests
// defined in this module. This parameter should only be used for testing and
// must not be set to true otherwise since it breaks security when it is
// enabled. By default and for safety reasons this parameter is set to false.
int uvm_enable_builtin_tests = 0;
module_param(uvm_enable_builtin_tests, int, S_IRUGO);
MODULE_PARM_DESC(uvm_enable_builtin_tests,
                 "Enable the UVM built-in tests. (This is a security risk)");

MODULE_LICENSE("MIT");
MODULE_INFO(supported, "external");
