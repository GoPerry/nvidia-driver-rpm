/* _NVRM_COPYRIGHT_BEGIN_
 *
 * Copyright 2014-2016 by NVIDIA Corporation.  All rights reserved.  All
 * information contained herein is proprietary and confidential to NVIDIA
 * Corporation.  Any use, reproduction, or disclosure without the written
 * permission of NVIDIA Corporation is prohibited.
 *
 * _NVRM_COPYRIGHT_END_
 */

#include <linux/module.h>
#include <linux/interrupt.h>
#include "conftest.h"

#include "nvlink_pci.h"
#include "nvlink_proto.h"
#include "nvlink_common.h"
#include "nvlink_errors.h"

#include "ibmnpu_export.h"

#if defined(NVCPU_PPC64LE) && defined(NV_LINUX_OF_H_PRESENT) && \
  defined(NV_OF_PARSE_PHANDLE_PRESENT)
#include <linux/of.h>

#ifndef IRQ_RETVAL
typedef void irqreturn_t;
#define IRQ_RETVAL(a)
#endif

#if !defined(IRQF_SHARED)
#define IRQF_SHARED SA_SHIRQ
#endif

static int              ibmnpu_probe    (struct pci_dev *, const struct pci_device_id *);
static void             ibmnpu_remove   (struct pci_dev *);
static pci_ers_result_t ibmnpu_pci_error_detected   (struct pci_dev *, enum pci_channel_state);
static pci_ers_result_t ibmnpu_pci_mmio_enabled     (struct pci_dev *);

static struct pci_error_handlers ibmnpu_pci_error_handlers =
{
    .error_detected = ibmnpu_pci_error_detected,
    .mmio_enabled   = ibmnpu_pci_mmio_enabled,
};

static struct pci_device_id ibmnpu_pci_table[] = {
    {
        .vendor      = PCI_VENDOR_ID_IBM,
        .device      = PCI_DEVICE_ID_IBM_NPU,
        .subvendor   = PCI_ANY_ID,
        .subdevice   = PCI_ANY_ID,
        .class       = (PCI_CLASS_BRIDGE_NPU << 8),
        .class_mask  = ~1
    },
    { }
};

static struct pci_driver ibmnpu_pci_driver = {
    .name           = IBMNPU_DRIVER_NAME,
    .id_table       = ibmnpu_pci_table,
    .probe          = ibmnpu_probe,
    .remove         = ibmnpu_remove,
    .err_handler    = &ibmnpu_pci_error_handlers,
};

/* Low-priority, preemptible watchdog for checking device status */
static struct {
    struct mutex        lock;
    struct delayed_work work;
    struct list_head    devices;
    NvBool              rearm;
} g_ibmnpu_watchdog;

typedef struct {
    struct pci_dev  *dev;
    struct list_head list_node;
} ibmnpu_watchdog_device;

static void ibmnpu_watchdog_check_devices(struct work_struct *);

static void ibmnpu_init_watchdog(void)
{
    mutex_init(&g_ibmnpu_watchdog.lock);
    INIT_DELAYED_WORK(&g_ibmnpu_watchdog.work, ibmnpu_watchdog_check_devices);
    INIT_LIST_HEAD(&g_ibmnpu_watchdog.devices);
    g_ibmnpu_watchdog.rearm = NV_TRUE;
}

static void ibmnpu_shutdown_watchdog(void)
{
    struct list_head *cur, *next;

    mutex_lock(&g_ibmnpu_watchdog.lock);

    g_ibmnpu_watchdog.rearm = NV_FALSE;

    mutex_unlock(&g_ibmnpu_watchdog.lock);

    /*
     * Wait to make sure the watchdog finishes its execution before proceeding
     * with the teardown.
     */
    flush_delayed_work(&g_ibmnpu_watchdog.work);

    mutex_lock(&g_ibmnpu_watchdog.lock);

    /*
     * Remove any remaining devices in the watchdog's check list (although
     * they should already have been removed in the typical case).
     */
    if (!list_empty(&g_ibmnpu_watchdog.devices))
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "IBMNPU: watchdog still running on devices:\n");
        list_for_each_safe(cur, next, &g_ibmnpu_watchdog.devices)
        {
            ibmnpu_watchdog_device *wd_dev =
                list_entry(cur, ibmnpu_watchdog_device, list_node);

            nvlink_print(NVLINK_DBG_ERRORS,
                "IBMNPU:    %04x:%02x:%02x.%x\n",
                NV_PCI_DOMAIN_NUMBER(wd_dev->dev),
                NV_PCI_BUS_NUMBER(wd_dev->dev),
                NV_PCI_SLOT_NUMBER(wd_dev->dev),
                PCI_FUNC(wd_dev->dev->devfn));

            list_del(cur);
            kfree(wd_dev);
        }
    }

    mutex_unlock(&g_ibmnpu_watchdog.lock);
}

/*
 * Add a device to the list of devices that the watchdog will periodically
 * check on. Start the watchdog if this is the first device to be registered.
 */
static NvlStatus ibmnpu_start_watchdog_device(struct pci_dev *dev)
{
    NvlStatus retval = NVL_SUCCESS;
    ibmnpu_watchdog_device *wd_dev;

    mutex_lock(&g_ibmnpu_watchdog.lock);

    wd_dev = kmalloc(sizeof(ibmnpu_watchdog_device), GFP_KERNEL);
    if (wd_dev != NULL)
    {
        wd_dev->dev = dev;
        list_add_tail(&wd_dev->list_node, &g_ibmnpu_watchdog.devices);
        if (list_is_singular(&g_ibmnpu_watchdog.devices))
        {
            /* Make the watchdog work item re-schedule itself */
            g_ibmnpu_watchdog.rearm = NV_TRUE;
            schedule_delayed_work(&g_ibmnpu_watchdog.work, HZ);
        }
    }
    else
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "IBMNPU: failed to allocate watchdog state for device %04x:%02x:%02x.%x\n",
            NV_PCI_DOMAIN_NUMBER(dev), NV_PCI_BUS_NUMBER(dev),
            NV_PCI_SLOT_NUMBER(dev), PCI_FUNC(dev->devfn));
        retval = -NVL_NO_MEM;
    }

    mutex_unlock(&g_ibmnpu_watchdog.lock);

    return retval;
}

/*
 * Stops the watchdog from checking the given device and waits for the
 * watchdog to finish, if no more devices need to be check.
 */
static void ibmnpu_stop_watchdog_device(struct pci_dev *dev)
{
    struct list_head *cur;

    mutex_lock(&g_ibmnpu_watchdog.lock);

    list_for_each(cur, &g_ibmnpu_watchdog.devices)
    {
        ibmnpu_watchdog_device *wd_dev =
            list_entry(cur, ibmnpu_watchdog_device, list_node);

        if (wd_dev->dev == dev)
        {
            list_del(cur);
            kfree(wd_dev);
            break;
        }
    }

    g_ibmnpu_watchdog.rearm = !list_empty(&g_ibmnpu_watchdog.devices);

    mutex_unlock(&g_ibmnpu_watchdog.lock);

    if (!g_ibmnpu_watchdog.rearm)
    {
        /*
         * Wait for the last work item to complete before proceeding with
         * the teardown. We must not hold the lock here so that the watchdog
         * work item can proceed.
         */
        flush_delayed_work(&g_ibmnpu_watchdog.work);
    }
}

/*
 * Periodic callback to check NPU devices for failure.
 *
 * This executes as a work item that re-schedules itself.
 */
static void ibmnpu_watchdog_check_devices
(
    struct work_struct * __always_unused work
)
{
    struct list_head *cur, *next;

    mutex_lock(&g_ibmnpu_watchdog.lock);

    list_for_each_safe(cur, next, &g_ibmnpu_watchdog.devices)
    {
        ibmnpu_watchdog_device *wd_dev =
            list_entry(cur, ibmnpu_watchdog_device, list_node);

        if (unlikely(ibmnpu_lib_check_failure(wd_dev->dev)))
        {
            /*
             * Mark the device as failed, and remove it from the watchdog's
             * check list. No need to print anything, since the EEH handler
             * ibmnpu_pci_error_detected() will have already been run for this
             * device.
             */
            list_del(cur);
            kfree(wd_dev);
        }
    }

    /*
     * Stop the watchdog from rescheduling itself if there are no more
     * devices left to check on.
     */
    if (unlikely(list_empty(&g_ibmnpu_watchdog.devices)))
    {
        g_ibmnpu_watchdog.rearm = NV_FALSE;
    }
    else if (likely(g_ibmnpu_watchdog.rearm))
    {
        schedule_delayed_work(&g_ibmnpu_watchdog.work, HZ);
    }

    mutex_unlock(&g_ibmnpu_watchdog.lock);
}

static irqreturn_t ibmnpu_isr
(
    int   irq,
    void *arg
#if !defined(NV_IRQ_HANDLER_T_PRESENT) || (NV_IRQ_HANDLER_T_ARGUMENT_COUNT == 3)
    ,struct pt_regs *regs
#endif
)
{
    nvlink_pci_info *info = (nvlink_pci_info *)arg;

    if (NULL == arg)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "An interrupt was fired for an NPU device, but no device info was "
            "provided\n");
        return IRQ_NONE;
    }

    nvlink_print(NVLINK_DBG_ERRORS,
        "IBMNPU: An interrupt has occurred on NPU device %04x:%02x:%02x.%x\n",
        info->domain, info->bus, info->device, info->function);

    ibmnpu_lib_service_device(info);

    return IRQ_HANDLED;
}

static int ibmnpu_probe
(
    struct pci_dev *dev,
    const struct pci_device_id *id_table
)
{
    NvlStatus retval = NVL_SUCCESS;

    nvlink_print(NVLINK_DBG_SETUP,
        "IBMNPU: Probing Emulated device %04x:%02x:%02x.%x, "
        "Vendor Id = 0x%x, Device Id = 0x%x, Class = 0x%x \n",
        NV_PCI_DOMAIN_NUMBER(dev), NV_PCI_BUS_NUMBER(dev),
        NV_PCI_SLOT_NUMBER(dev), PCI_FUNC(dev->devfn),
        dev->vendor, dev->device, dev->class);

    // Try to register the device in nvlink core library
    retval = ibmnpu_lib_register_device(NV_PCI_DOMAIN_NUMBER(dev),
                                        NV_PCI_BUS_NUMBER(dev),
                                        NV_PCI_SLOT_NUMBER(dev),
                                        PCI_FUNC(dev->devfn),
                                        dev);

    // If there is no GPU associated with this NPU, skip it.
    if (NVL_UNBOUND_DEVICE == retval)
    {
        nvlink_print(NVLINK_DBG_SETUP,
            "IBMNPU: No GPU is associated to this brick, skipping.\n");
        return -ENODEV;
    }

    if (NVL_SUCCESS != retval)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "Failed to register NPU device : %d\n", retval);

        goto register_device_fail;
    }

    if (dev->irq == 0)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "IBMNPU: Can't find an IRQ!\n");
    }

    return 0;

register_device_fail:

    return -1;
}

void ibmnpu_remove(struct pci_dev *dev)
{
    // TODO not supported yet

    nvlink_print(NVLINK_DBG_SETUP,
        "IBMNPU: removing device %04x:%02x:%02x.%x\n",
        NV_PCI_DOMAIN_NUMBER(dev), NV_PCI_BUS_NUMBER(dev),
        NV_PCI_SLOT_NUMBER(dev), PCI_FUNC(dev->devfn));
    return;
}

static pci_ers_result_t ibmnpu_pci_error_detected
(
    struct pci_dev *dev,
    enum pci_channel_state error
)
{
    nvlink_pci_info *pci_info;

    if (NULL == dev)
    {
        return PCI_ERS_RESULT_NONE;
    }

    pci_info = pci_get_drvdata(dev);

    nvlink_print(NVLINK_DBG_ERRORS,
        "IBMNPU: ibmnpu_pci_error_detected device %04x:%02x:%02x.%x\n",
        NV_PCI_DOMAIN_NUMBER(dev), NV_PCI_BUS_NUMBER(dev),
        NV_PCI_SLOT_NUMBER(dev), PCI_FUNC(dev->devfn));

    // Mark the device as off-limits
    ibmnpu_lib_stop_device_mmio(pci_info);

    if (pci_channel_io_perm_failure == error)
    {
        return PCI_ERS_RESULT_DISCONNECT;
    }

    //
    // For NPU devices we need to determine if its FREEZE/FENCE EEH, which
    // requires a register read.
    // Tell Linux to continue recovery of the device. The kernel will enable
    // MMIO for the NPU and call the mmio_enabled callback.
    //
    return PCI_ERS_RESULT_CAN_RECOVER;
}

static pci_ers_result_t ibmnpu_pci_mmio_enabled
(
    struct pci_dev *dev
)
{
    if (NULL == dev)
    {
        return PCI_ERS_RESULT_NONE;
    }

    nvlink_print(NVLINK_DBG_ERRORS,
        "IBMNPU: ibmnpu_pci_mmio_enabled device %04x:%02x:%02x.%x\n",
        NV_PCI_DOMAIN_NUMBER(dev), NV_PCI_BUS_NUMBER(dev),
        NV_PCI_SLOT_NUMBER(dev), PCI_FUNC(dev->devfn));

    //
    // It is understood that we will not attempt to recover from an EEH, but 
    // IBM has requested that we indicate in the logs that it occured and
    // that it was either a FREEZE or a FENCE.
    //
    // Within the MMIO handler specifically, a persistent failure condition
    // is considered a FENCE condition which requires a system power cycle.
    //
    if (ibmnpu_lib_check_failure(dev))
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "IBMNPU: NPU FENCE detected, machine power cycle required.\n");
    }
    else
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "IBMNPU: NPU FREEZE detected, driver reload required.\n");
    }

    nvlink_print(NVLINK_DBG_ERRORS,
        "IBMNPU: Disconnecting device %04x:%02x:%02x.%x\n",
        NV_PCI_DOMAIN_NUMBER(dev), NV_PCI_BUS_NUMBER(dev),
        NV_PCI_SLOT_NUMBER(dev), PCI_FUNC(dev->devfn));

    // There is no way out at this point, request a disconnect.
    return PCI_ERS_RESULT_DISCONNECT;
}

NvBool NVLINK_API_CALL ibmnpu_lib_check_failure(void *handle)
{
    NvU16 pci_vendor; 

    //
    // According to IBM, any config cycle read of all Fs will cause the
    // firmware to check for an EEH failure on the associated device.
    // If the EEH failure condition exists, EEH error handling will be
    // triggered and PCIBIOS_DEVICE_NOT_FOUND will be returned.    
    //
    return (pci_read_config_word(handle, PCI_VENDOR_ID, &pci_vendor) == PCIBIOS_DEVICE_NOT_FOUND) ? NV_TRUE : NV_FALSE;
}

int ibmnpu_init(void)
{
    NvlStatus retval = ibmnpu_lib_load(0xFFFFFFFF, 0xFFFFFFFF);

    if (NVL_SUCCESS != retval)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "Failed to load ibmnpu library : %d\n", retval);
        return -1;
    }

    return 0;
}

void ibmnpu_exit(void)
{
    NvlStatus retval = ibmnpu_lib_unload();

    if (NVL_SUCCESS != retval)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "Error occurred while unloading ibmnpu library : %d\n", retval);
    }
}



NvlStatus NVLINK_API_CALL ibmnpu_lib_load
(
    NvU32 accepted_domain,
    NvU32 accepted_link_mask
)
{
    NvlStatus retval = NVL_SUCCESS;
    int rc;

    ibmnpu_init_watchdog();

    retval = ibmnpu_lib_initialize(accepted_domain, accepted_link_mask);
    if (NVL_SUCCESS != retval)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "Failed to initialize ibmnpu driver : %d\n", retval);

        goto ibmnpu_lib_initialize_fail;
    }

    rc = pci_register_driver(&ibmnpu_pci_driver);
    if (rc < 0)
    {
        nvlink_print(NVLINK_DBG_ERRORS, 
            "Failed to register ibmnpu driver : %d\n", rc);

        retval = (NvlStatus)rc;
        goto pci_register_driver_fail;
    }

    return retval;

pci_register_driver_fail:
    ibmnpu_lib_shutdown();

ibmnpu_lib_initialize_fail:
    ibmnpu_shutdown_watchdog();

    return retval;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_initialize_device_pci_bus(void *handle)
{
    NvlStatus retval = NVL_SUCCESS;
    int rc;

    if (NULL == handle)
    {
        return -NVL_BAD_ARGS;
    }

    rc = pci_enable_device(handle);
    if (0 != rc)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "Failed to enable pci device : %d\n", rc);

        return (NvlStatus)rc;
    }

    // Enable bus mastering on the device
    pci_set_master(handle);

    return retval;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_initialize_device_bar_info(void *handle, nvlink_pci_info *info)
{
    NvlStatus retval = NVL_SUCCESS;
    struct pci_dev *dev = handle;
    unsigned int i, j;
    int rc;

    if (NULL == handle || NULL == info)
    {
        return -NVL_BAD_ARGS;
    }

    if (NULL != info->bars[0].pBar)
    {
        nvlink_print(NVLINK_DBG_WARNINGS,
            "Cannot map ibmnpu device registers : already initialized.\n");
        return retval;
    }

    rc = pci_request_regions(dev, IBMNPU_DRIVER_NAME);
    if (rc != 0)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "Failed to request memory regions : %d\n", rc);

        return (NvlStatus)rc;
    }

    for (i = 0, j = 0; i < NVRM_PCICFG_NUM_BARS && j < IBMNPU_MAX_BARS; i++)
    {
        if ((NV_PCI_RESOURCE_VALID(dev, i)) &&
            (NV_PCI_RESOURCE_FLAGS(dev, i) & PCI_BASE_ADDRESS_SPACE)
                == PCI_BASE_ADDRESS_SPACE_MEMORY)
        {
            NvU32 bar = 0;

            info->bars[j].offset = NVRM_PCICFG_BAR_OFFSET(i);
            pci_read_config_dword(dev, info->bars[j].offset, &bar);
            info->bars[j].busAddress = (bar & PCI_BASE_ADDRESS_MEM_MASK);
            if (NV_PCI_RESOURCE_FLAGS(dev, i) & PCI_BASE_ADDRESS_MEM_TYPE_64)
            {
                pci_read_config_dword(dev, info->bars[j].offset + 4, &bar);
                info->bars[j].busAddress |= (((NvU64)bar) << 32);
            }


            info->bars[j].baseAddr = NV_PCI_RESOURCE_START(dev, i);
            info->bars[j].barSize = NV_PCI_RESOURCE_SIZE(dev, i);

            nvlink_print(NVLINK_DBG_INFO,
                "IBMNPU: Bar%d @ 0x%llx [size=%dK].\n",
                j, info->bars[j].baseAddr, (info->bars[j].barSize >> 10));

            // Map registers to kernel address space.
            info->bars[j].pBar = pci_iomap(dev, i, 0);
            if (NULL == info->bars[j].pBar)
            {
                nvlink_print(NVLINK_DBG_ERRORS,
                    "IBMNPU: Unable to map BAR%d registers\n", j);
                
                retval = -NVL_PCI_ERROR;
                goto pci_iomap_fail;
            }
            j++;
        }
    }

    pci_set_drvdata(dev, info);

    retval = ibmnpu_start_watchdog_device(handle);
    if (retval != NVL_SUCCESS)
    {
        goto ibmnpu_start_watchdog_device_fail;
    }

    return retval;

ibmnpu_start_watchdog_device_fail:
pci_iomap_fail:
    ibmnpu_lib_shutdown_device_bar_info(dev, info);

    return retval;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_unload(void)
{
    NvlStatus retval;

    retval = ibmnpu_lib_shutdown();
    if (NVL_SUCCESS != retval)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "Failed to shutdown ibmnpu driver : %d\n", retval);
    }

    ibmnpu_shutdown_watchdog();

    pci_unregister_driver(&ibmnpu_pci_driver);

    return retval;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_initialize_device_interrupt(void *handle, nvlink_pci_info *info)
{
    struct pci_dev *dev = handle;
    int rc;

    if (NULL == handle || NULL == info)
    {
        return -NVL_BAD_ARGS;
    }

    if (info->intHooked)
    {
        nvlink_print(NVLINK_DBG_SETUP,
            "ibmnpu interrupt already initialized\n");
        return NVL_SUCCESS;
    }

    info->irq = dev->irq;

    rc = request_irq(info->irq, ibmnpu_isr, IRQF_SHARED, IBMNPU_DEVICE_NAME,
                     (void *)info);
    if (rc != 0)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "NPU device failed to get irq (%d)\n", rc);
        return -NVL_PCI_ERROR;
    }

    info->intHooked = NV_TRUE;

    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_shutdown_device_interrupt(void *handle, nvlink_pci_info *info)
{
    if (NULL == handle || NULL == info)
    {
        return -NVL_BAD_ARGS;
    }

    if (!info->intHooked)
    {
        nvlink_print(NVLINK_DBG_SETUP, "ibmnpu interrupt not wired up\n");
        return NVL_SUCCESS;
    }

    free_irq(info->irq, (void *)info);

    info->intHooked = NV_FALSE;

    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_shutdown_device_bar_info(void *handle, nvlink_pci_info *info)
{
    NvlStatus retval = NVL_SUCCESS;
    unsigned int bar;

    if (NULL == handle || NULL == info)
    {
        return -NVL_BAD_ARGS;
    }

    if (NULL == info->bars[0].pBar)
    {
        nvlink_print(NVLINK_DBG_WARNINGS,
            "Cannot unmap ibmnpu device bars: not initialized.\n");
        return retval;
    }

    ibmnpu_stop_watchdog_device(handle);

    pci_set_drvdata(handle, NULL);
    pci_release_regions(handle);

    for (bar = 0; bar < IBMNPU_MAX_BARS; bar++)
    {
        if (NULL != info->bars[bar].pBar)
        {
            pci_iounmap(handle, info->bars[bar].pBar);
            info->bars[bar].pBar = NULL;
        }
    }

    return retval;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_shutdown_device_pci_bus(void *handle)
{
    if (NULL == handle)
    {
        return -NVL_BAD_ARGS;
    }

    pci_disable_device(handle);

    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_release_device(void *handle)
{
    return NVL_SUCCESS;
}

NvU8 NVLINK_API_CALL ibmnpu_lib_pci_read_08 (void *handle, NvU32 offset)
{
    NvU8 buffer = 0xFF;
    if (NULL == handle || offset > NV_PCIE_CFG_MAX_OFFSET)
    {
        return buffer;
    }

    pci_read_config_byte(handle, offset, &buffer);
    return buffer;
}

NvU16 NVLINK_API_CALL ibmnpu_lib_pci_read_16 (void *handle, NvU32 offset)
{
    NvU16 buffer = 0xFFFF;
    if (NULL == handle || offset > NV_PCIE_CFG_MAX_OFFSET)
    {
        return buffer;
    }

    pci_read_config_word(handle, offset, &buffer);
    return buffer;
}

NvU32 NVLINK_API_CALL ibmnpu_lib_pci_read_32 (void *handle, NvU32 offset)
{
    NvU32 buffer = 0xFFFFFFFF;
    if (NULL == handle || offset > NV_PCIE_CFG_MAX_OFFSET)
    {
        return buffer;
    }

    pci_read_config_dword(handle, offset, &buffer);
    return buffer;
}

void  NVLINK_API_CALL ibmnpu_lib_pci_write_08(void *handle, NvU32 offset, NvU8  data)
{
    if (NULL == handle || offset > NV_PCIE_CFG_MAX_OFFSET)
    {
        return;
    }
    
    pci_write_config_byte(handle, offset, data);
}

void  NVLINK_API_CALL ibmnpu_lib_pci_write_16(void *handle, NvU32 offset, NvU16 data)
{
    if (NULL == handle || offset > NV_PCIE_CFG_MAX_OFFSET)
    {
        return;
    }
    
    pci_write_config_word(handle, offset, data);
}

void  NVLINK_API_CALL ibmnpu_lib_pci_write_32(void *handle, NvU32 offset, NvU32 data)
{
    if (NULL == handle || offset > NV_PCIE_CFG_MAX_OFFSET)
    {
        return;
    }
    
     pci_write_config_dword(handle, offset, data);
}

#else

int ibmnpu_init(void)
{
    return 0;
}

void ibmnpu_exit(void)
{
    return;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_load
(
    NvU32 accepted_domain,
    NvU32 accepted_link_mask
)
{
    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_initialize_device_pci_bus(void *handle)
{
    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_initialize_device_bar_info(void *handle, nvlink_pci_info *info)
{
    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_initialize_device_interrupt(void *handle, nvlink_pci_info *info)
{
    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_shutdown_device_interrupt(void *handle, nvlink_pci_info *info)
{
    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_unload(void)
{
    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_shutdown_device_bar_info(void *handle, nvlink_pci_info *info)
{
    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_shutdown_device_pci_bus(void *handle)
{
    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ibmnpu_lib_release_device(void *handle)
{
    return NVL_SUCCESS;
}

NvU8  NVLINK_API_CALL ibmnpu_lib_pci_read_08 (void *handle, NvU32 offset)
{
    return 0;
}

NvU16 NVLINK_API_CALL ibmnpu_lib_pci_read_16 (void *handle, NvU32 offset)
{
    return 0;
}

NvU32 NVLINK_API_CALL ibmnpu_lib_pci_read_32 (void *handle, NvU32 offset)
{
    return 0;
}

void  NVLINK_API_CALL ibmnpu_lib_pci_write_08(void *handle, NvU32 offset, NvU8  data)
{
    return;
}

void  NVLINK_API_CALL ibmnpu_lib_pci_write_16(void *handle, NvU32 offset, NvU16 data)
{
    return;
}

void  NVLINK_API_CALL ibmnpu_lib_pci_write_32(void *handle, NvU32 offset, NvU32 data)
{
    return;
}

NvBool NVLINK_API_CALL ibmnpu_lib_check_failure(void *handle)
{
    return 0;
}

#endif
