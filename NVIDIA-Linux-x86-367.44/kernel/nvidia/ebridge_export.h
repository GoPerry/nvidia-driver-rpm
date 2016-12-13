/*******************************************************************************
    Copyright (c) 2015 NVidia Corporation

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


#ifndef _EBRIDGE_EXPORT_H_
#define _EBRIDGE_EXPORT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "nvlink_common.h"


/*
 * @Brief : Initializes and registers the Ebridge driver with NVlink.
 *
 * @Description :
 *
 * @returns                 NVL_SUCCESS if action succeeded,
 *                          an NVL error code otherwise
 */
NvlStatus NVLINK_API_CALL ebridge_lib_initialize(void);

/*
 * @Brief : Shuts down and unregisters the driver/devices from the NVlink
 *              library.
 *
 * @Description :
 *
 * @returns                 NVL_SUCCESS if the action succeeded
 *                          an NVL error code otherwise
 */
NvlStatus NVLINK_API_CALL ebridge_lib_shutdown(void);

/*
 * @Brief : Creates and registers a device with the given data with the nvlink
 *              core library.
 *
 * @Description :
 *
 * @param[in] domain        pci domain of the device
 * @param[in] bus           pci bus of the device
 * @param[in] device        pci device of the device
 * @param[in] func          pci function of the device
 * @param[in] handle        Device handle used to interact with arch layer
 *
 * @returns                 NVL_SUCCESS if the action succeeded
 *                          an NVL error code otherwise
 */
NvlStatus NVLINK_API_CALL ebridge_lib_register_device
(
    NvU16 domain, 
    NvU8 bus, 
    NvU8 device, 
    NvU8 func,
    void *handle
);


/*
 * @Brief: Loads PCI devices matching the Ebridge profile into the driver.
 *
 * @Description :
 *
 * @returns                 NVL_SUCCESS if the action succeeded
 *                          An NVL error code otherwise.
 *                          Output prints will show if registration failed
 *                          for some devices.
 */
// XXX moving to _load and _unload
NvlStatus ebridge_lib_find_devices(void);

/*
 * Initializes ebridge library, preparing the driver to register
 *     discovered devices into the core library.
 */
NvlStatus NVLINK_API_CALL ebridge_lib_load(void);

/*
 * Initializes the pci bus for the given device, including
 *     enabling device memory transactions and bus mastering.
 */
NvlStatus NVLINK_API_CALL ebridge_lib_initialize_device_pci_bus(void *handle);

/*
 * Maps the device base address registers into CPU memory, and
 *     populates the device pci data with the mapping.
 */
NvlStatus NVLINK_API_CALL ebridge_lib_initialize_device_bar_info
(
    void *handle,
    nvlink_pci_info *info
);

/*
 * Shuts down the ebridge library, deregistering its devices from
 *     the core and freeing core operating system accounting info.
 */
NvlStatus NVLINK_API_CALL ebridge_lib_unload(void);

/*
 * Unmaps the previously mapped base address registers from cpu memory.
 */
NvlStatus NVLINK_API_CALL ebridge_lib_shutdown_device_bar_info(void *handle, nvlink_pci_info *info);

/*
 * Cleans up any state the arch layer allocated for this device.
 */
NvlStatus NVLINK_API_CALL ebridge_lib_release_device(void *handle);

NvU8  NVLINK_API_CALL ebridge_lib_pci_read_08 (void *handle, NvU32 offset);
NvU16 NVLINK_API_CALL ebridge_lib_pci_read_16 (void *handle, NvU32 offset);
NvU32 NVLINK_API_CALL ebridge_lib_pci_read_32 (void *handle, NvU32 offset);
void  NVLINK_API_CALL ebridge_lib_pci_write_08(void *handle, NvU32 offset, NvU8  data);
void  NVLINK_API_CALL ebridge_lib_pci_write_16(void *handle, NvU32 offset, NvU16 data);
void  NVLINK_API_CALL ebridge_lib_pci_write_32(void *handle, NvU32 offset, NvU32 data);

#ifdef __cplusplus
}
#endif

#endif //_EBRIDGE_EXPORT_H_
