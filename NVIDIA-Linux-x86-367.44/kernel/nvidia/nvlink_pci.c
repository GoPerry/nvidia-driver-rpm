/* _NVRM_COPYRIGHT_BEGIN_
 *
 * Copyright 2015-2016 by NVIDIA Corporation.  All rights reserved.  All
 * information contained herein is proprietary and confidential to NVIDIA
 * Corporation.  Any use, reproduction, or disclosure without the written
 * permission of NVIDIA Corporation is prohibited.
 *
 * _NVRM_COPYRIGHT_END_
 */

#include "nvlink_pci.h"

int nv_pci_disable_device(struct pci_dev *dev)
{
    int rc;
    NvU16 __cmd[2];

    rc = pci_read_config_word((dev), PCI_COMMAND, &__cmd[0]);
    if (rc < 0)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "IBMNPU: pci_read_config_word failed with error %x\n", rc);
    }

    pci_disable_device(dev);

    rc = pci_read_config_word((dev), PCI_COMMAND, &__cmd[1]);
    if (rc < 0)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "IBMNPU: pci_read_config_word failed with error %x\n", rc);
    }

    __cmd[1] |= PCI_COMMAND_MEMORY;
    rc = pci_write_config_word((dev), PCI_COMMAND,
             (__cmd[1] | (__cmd[0] & PCI_COMMAND_IO)));
    if (rc < 0)
    {
        nvlink_print(NVLINK_DBG_ERRORS,
            "IBMNPU: pci_write_config_word failed with error %x\n", rc);
    }

    return rc;
}

NvU8 NVLINK_API_CALL nvlink_pciCfgRd08
(
    NvU16 domain,
    NvU8 bus,
    NvU8 device,
    NvU8 function,
    NvU32 offset
)
{
    //TODO provide linux support for pci cfg rd/wr
    return 0xFF;
}

NvU16 NVLINK_API_CALL nvlink_pciCfgRd16
(
    NvU16 domain,
    NvU8 bus,
    NvU8 device,
    NvU8 function,
    NvU32 offset
)
{
    //TODO provide linux support for pci cfg rd/wr
    return 0xFFFF;
}

NvU32 NVLINK_API_CALL nvlink_pciCfgRd32
(
    NvU16 domain,
    NvU8 bus,
    NvU8 device,
    NvU8 function,
    NvU32 offset
)
{
    //TODO provide linux support for pci cfg rd/wr
    return 0xFFFFFFFF;
}

void NVLINK_API_CALL nvlink_pciCfgWr08
(
    NvU16 domain,
    NvU8 bus,
    NvU8 device,
    NvU8 function,
    NvU32 offset,
    NvU8 data
)
{
    //TODO provide linux support for pci cfg rd/wr
}

void NVLINK_API_CALL nvlink_pciCfgWr16
(
    NvU16 domain,
    NvU8 bus,
    NvU8 device,
    NvU8 function,
    NvU32 offset,
    NvU16 data
)
{
    //TODO provide linux support for pci cfg rd/wr
}

void NVLINK_API_CALL nvlink_pciCfgWr32
(
    NvU16 domain,
    NvU8 bus,
    NvU8 device,
    NvU8 function,
    NvU32 offset,
    NvU32 data
)
{
    //TODO provide linux support for pci cfg rd/wr
}

