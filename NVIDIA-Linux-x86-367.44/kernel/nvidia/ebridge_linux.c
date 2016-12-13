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

#include "nvlink_pci.h"
#include "nvlink_proto.h"
#include "nvlink_common.h"
#include "nvlink_errors.h"
#include "conftest.h"
#include "ebridge_export.h"

#define EBRIDGE_DEV_NAME "ebridge"

#define NV_PCI_DEVICE_ID_EBRIDGE_1   0x10EC
#define NV_PCI_DEVICE_ID_EBRIDGE_2   0x10ED

#define EBRIDGE_MAX_BARS              1
#define EBRIDGE_BAR_INDEX_REGS1       0

#ifndef IRQ_RETVAL
typedef void irqreturn_t;
#define IRQ_RETVAL(a)
#endif

#if !defined(IRQF_SHARED)
#define IRQF_SHARED SA_SHIRQ
#endif

static int           ebridge_probe    (struct pci_dev *, const struct pci_device_id *);
static void          ebridge_remove   (struct pci_dev *);
#if !defined(NV_IRQ_HANDLER_T_PRESENT) || (NV_IRQ_HANDLER_T_ARGUMENT_COUNT == 3)
static irqreturn_t   ebridge_isr    (int, void *, struct pt_regs *);
#else
static irqreturn_t   ebridge_isr    (int, void *);
#endif

static struct pci_device_id ebridge_pci_table[] = {
    {
        .vendor      = PCI_VENDOR_ID_NVIDIA,
        .device      = NV_PCI_DEVICE_ID_EBRIDGE_1,
        .subvendor   = PCI_ANY_ID,
        .subdevice   = PCI_ANY_ID,
        .class       = (PCI_CLASS_BRIDGE_OTHER << 8),
        .class_mask  = ~0
    },
    {
        .vendor      = PCI_VENDOR_ID_NVIDIA,
        .device      = NV_PCI_DEVICE_ID_EBRIDGE_2,
        .subvendor   = PCI_ANY_ID,
        .subdevice   = PCI_ANY_ID,
        .class       = (PCI_CLASS_BRIDGE_OTHER << 8),
        .class_mask  = ~0
    },
    { }
};

static struct pci_driver ebridge_pci_driver = {
    .name     = EBRIDGE_DEV_NAME,
    .id_table = ebridge_pci_table,
    .probe    = ebridge_probe,
    .remove   = ebridge_remove,
};

static int
ebridge_probe
(
    struct pci_dev *dev,
    const struct pci_device_id *id_table
)
{
    nvlink_dev_linux_state_t *nvls;
    unsigned int i, j;
    int rc;

    nvlink_print(NVLINK_DBG_SETUP,
        "EBRIDGE: probing 0x%x 0x%x, class 0x%x\n",
        dev->vendor, dev->device, dev->class);

    if (pci_enable_device(dev) != 0)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "EBRIDGE: pci_enable_device failed, aborting\n");
        goto failed;
    }

    if (dev->irq == 0)
    {
        nvlink_print(NVLINK_DBG_ERRORS, "EBRIDGE: Can't find an IRQ!\n");
        goto failed;
    }

    if (!request_mem_region(NV_PCI_RESOURCE_START(dev, EBRIDGE_BAR_INDEX_REGS1),
                            NV_PCI_RESOURCE_SIZE(dev, EBRIDGE_BAR_INDEX_REGS1),
                            EBRIDGE_DEV_NAME))
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "EBRIDGE: request_mem_region failed for %dM @ 0x%llx.\n",
            (NV_PCI_RESOURCE_SIZE(dev, EBRIDGE_BAR_INDEX_REGS1) >> 20),
            (NvU64)NV_PCI_RESOURCE_START(dev, EBRIDGE_BAR_INDEX_REGS1));
        goto failed;
    }

    nvls = (nvlink_dev_linux_state_t *) nvlink_malloc(sizeof(nvlink_dev_linux_state_t));
    if (nvls == NULL)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "EBRIDGE: failed to allocate memory\n");
        goto err_not_supported;
    }

    nvlink_memset(nvls, 0, sizeof(nvlink_dev_linux_state_t));

    pci_set_drvdata(dev, (void *)nvls);

    nvls->dev                  = dev;
    nvls->pci_info.pciDeviceId = dev->device;
    nvls->pci_info.domain      = NV_PCI_DOMAIN_NUMBER(dev);
    nvls->pci_info.bus         = NV_PCI_BUS_NUMBER(dev);
    nvls->pci_info.device      = NV_PCI_SLOT_NUMBER(dev);
    nvls->pci_info.function    = PCI_FUNC(dev->devfn);

    for (i = 0, j = 0; i < NVRM_PCICFG_NUM_BARS && j < EBRIDGE_MAX_BARS; i++)
    {
        if ((NV_PCI_RESOURCE_VALID(dev, i)) &&
            (NV_PCI_RESOURCE_FLAGS(dev, i) & PCI_BASE_ADDRESS_SPACE)
                == PCI_BASE_ADDRESS_SPACE_MEMORY)
        {
            NvU32 bar = 0;
            nvls->pci_info.bars[j].offset = NVRM_PCICFG_BAR_OFFSET(i);
            pci_read_config_dword(dev, nvls->pci_info.bars[j].offset, &bar);
            nvls->pci_info.bars[j].busAddress = (bar & PCI_BASE_ADDRESS_MEM_MASK);
            if (NV_PCI_RESOURCE_FLAGS(dev, i) & PCI_BASE_ADDRESS_MEM_TYPE_64)
            {
                pci_read_config_dword(dev, nvls->pci_info.bars[j].offset + 4, &bar);
                nvls->pci_info.bars[j].busAddress |= (((NvU64)bar) << 32);
            }
            nvls->pci_info.bars[j].baseAddr = NV_PCI_RESOURCE_START(dev, i);
            nvls->pci_info.bars[j].barSize = NV_PCI_RESOURCE_SIZE(dev, i);
            nvlink_print(NVLINK_DBG_INFO,
                "EBRIDGE: Bar%d @ 0x%llx [size=%dK].\n",
                j, nvls->pci_info.bars[j].baseAddr,
                (nvls->pci_info.bars[j].barSize >> 10));
            j++;
        }
    }

    nvls->interrupt_line = dev->irq;

    pci_set_master(dev);

    rc = request_irq(nvls->interrupt_line, ebridge_isr,
                     IRQF_SHARED, EBRIDGE_DEV_NAME, (void *)nvls);
    if (rc != 0)
    {
        if ((nvls->interrupt_line != 0) && (rc == -EBUSY))
        {
            nvlink_print(NVLINK_DBG_ERRORS,
                "EBRIDGE: Tried to get IRQ %d, but another driver"
                "has it and is not sharing it.\n",
                (unsigned int) nvls->interrupt_line);
        }
        nvlink_print(NVLINK_DBG_ERRORS,
            "EBRIDGE: request_irq() failed (%d)\n", rc);
        goto err_not_supported;
    }

    //TODO: Map registers to kernel address space.

    return 0;

err_not_supported:
    //TODO: Unmap registers from kernel address space.
    release_mem_region(NV_PCI_RESOURCE_START(dev, EBRIDGE_BAR_INDEX_REGS1),
                       NV_PCI_RESOURCE_SIZE(dev, EBRIDGE_BAR_INDEX_REGS1));
    nv_pci_disable_device(dev);
    pci_set_drvdata(dev, NULL);
    if (nvls != NULL)
    {
        nvlink_free(nvls);
    }
failed:
    return -1;
}

void ebridge_remove(struct pci_dev *dev)
{
    nvlink_dev_linux_state_t *nvls = NULL;


    nvlink_print(NVLINK_DBG_SETUP,
        "EBRIDGE: removing device %04x:%02x:%02x.%x\n",
        NV_PCI_DOMAIN_NUMBER(dev), NV_PCI_BUS_NUMBER(dev),
        NV_PCI_SLOT_NUMBER(dev), PCI_FUNC(dev->devfn));

    nvls = pci_get_drvdata(dev);
    if (!nvls || (nvls->dev != dev))
    {
        goto done;
    }

    //TODO: Unmap registers from kernel address space

    free_irq(nvls->interrupt_line, nvls);

done:
    release_mem_region(NV_PCI_RESOURCE_START(dev, EBRIDGE_BAR_INDEX_REGS1),
                       NV_PCI_RESOURCE_SIZE(dev, EBRIDGE_BAR_INDEX_REGS1));
    nv_pci_disable_device(dev);
    pci_set_drvdata(dev, NULL);

    if (nvls != NULL)
    {
        nvlink_free(nvls);
    }
    return;
}

static irqreturn_t
ebridge_isr(
    int   irq,
    void *arg
#if !defined(NV_IRQ_HANDLER_T_PRESENT) || (NV_IRQ_HANDLER_T_ARGUMENT_COUNT == 3)
    ,struct pt_regs *regs
#endif
)
{
    NvBool rm_handled = NV_FALSE;

    //TODO: Route the interrupt to RM.

    return IRQ_RETVAL(rm_handled);
}

int ebridge_init(void)
{
    int rc;

    rc = pci_register_driver(&ebridge_pci_driver);
    if (rc < 0)
    {
        nvlink_print(NVLINK_DBG_INFO, "EBRIDGE: No device found!\n");
        goto out;
    }

out:
    return rc;
}

void ebridge_exit(void)
{
    pci_unregister_driver(&ebridge_pci_driver);
}


NvlStatus NVLINK_API_CALL ebridge_lib_load(void)
{
    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ebridge_lib_initialize_device_pci_bus(void *handle)
{
    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ebridge_lib_initialize_device_bar_info(void *handle, nvlink_pci_info *info)
{
    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ebridge_lib_shutdown_device_bar_info(void *handle, nvlink_pci_info *info)
{
    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ebridge_lib_unload(void)
{
    return NVL_SUCCESS;
}

NvlStatus NVLINK_API_CALL ebridge_lib_release_device(void *handle)
{
    return NVL_SUCCESS;
}

NvU8  NVLINK_API_CALL ebridge_lib_pci_read_08 (void *handle, NvU32 offset)
{
    return 0;
}

NvU16 NVLINK_API_CALL ebridge_lib_pci_read_16 (void *handle, NvU32 offset)
{
    return 0;
}

NvU32 NVLINK_API_CALL ebridge_lib_pci_read_32 (void *handle, NvU32 offset)
{
    return 0;
}

void  NVLINK_API_CALL ebridge_lib_pci_write_08(void *handle, NvU32 offset, NvU8  data)
{
    return;
}

void  NVLINK_API_CALL ebridge_lib_pci_write_16(void *handle, NvU32 offset, NvU16 data)
{
    return;
}

void  NVLINK_API_CALL ebridge_lib_pci_write_32(void *handle, NvU32 offset, NvU32 data)
{
    return;
}

