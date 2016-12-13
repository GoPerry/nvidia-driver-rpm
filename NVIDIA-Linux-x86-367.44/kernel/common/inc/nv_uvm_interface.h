/* _NVRM_COPYRIGHT_BEGIN_
 *
 * Copyright 2013-2015 by NVIDIA Corporation.  All rights reserved.  All
 * information contained herein is proprietary and confidential to NVIDIA
 * Corporation.  Any use, reproduction, or disclosure without the written
 * permission of NVIDIA Corporation is prohibited.
 *
 * _NVRM_COPYRIGHT_END_
 */

//
// This file provides the interface that RM exposes to UVM.
//

#ifndef _NV_UVM_INTERFACE_H_
#define _NV_UVM_INTERFACE_H_

// Forward references, to break circular header file dependencies:
struct UvmOpsUvmEvents;

//
// TODO (bug 1359805): This should all be greatly simplified. It is still
// carrying along a lot of baggage from when RM depended on UVM. Now that
// direction is reversed: RM is independent, and UVM depends on RM.
//
#if defined(NVIDIA_UVM_ENABLED)

// We are in the UVM build system, for a Linux target.
#include "uvmtypes.h"
#include "uvm_linux.h"

#else

// We are in the RM build system, for a Linux target:
#include "nv-linux.h"

#endif // NVIDIA_UVM_ENABLED

#include "nvgputypes.h"
#include "nvstatus.h"
#include "nv_uvm_types.h"

// Define the type here as it's Linux specific, used only by the Linux specific
// nvUvmInterfaceRegisterGpu() API.
typedef struct
{
    struct pci_dev *pci_dev;

    // DMA addressable range of the device, mirrors fields in nv_state_t.
    NvU64 dma_addressable_start;
    NvU64 dma_addressable_limit;
} UvmGpuPlatformInfo;

/*******************************************************************************
    nvUvmInterfaceRegisterGpu

    Registers the GPU with the provided UUID for use. A GPU must be registered
    before its UUID can be used with any other API. This call is ref-counted so
    every nvUvmInterfaceRegisterGpu must be paired with a corresponding
    nvUvmInterfaceUnregisterGpu.

    You don't need to call nvUvmInterfaceSessionCreate before calling this.

    Error codes:
        NV_ERR_GPU_UUID_NOT_FOUND
        NV_ERR_NO_MEMORY
        NV_ERR_GENERIC
*/
NV_STATUS nvUvmInterfaceRegisterGpu(NvProcessorUuid *gpuUuid, UvmGpuPlatformInfo *gpuInfo);

/*******************************************************************************
    nvUvmInterfaceUnregisterGpu

    Unregisters the GPU with the provided UUID. This drops the ref count from
    nvUvmInterfaceRegisterGpu. Once the reference count goes to 0 the device may
    no longer be accessible until the next nvUvmInterfaceRegisterGpu call. No
    automatic resource freeing is performed, so only make the last unregister
    call after destroying all your allocations associated with that UUID (such
    as those from nvUvmInterfaceAddressSpaceCreate).

    If the UUID is not found, no operation is performed.
*/
void nvUvmInterfaceUnregisterGpu(NvProcessorUuid *gpuUuid);

/*******************************************************************************
    nvUvmInterfaceSessionCreate

    TODO: Creates session object.  All allocations are tied to the session.

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_NO_MEMORY
*/
NV_STATUS nvUvmInterfaceSessionCreate(uvmGpuSessionHandle *session);

/*******************************************************************************
    nvUvmInterfaceSessionDestroy

    Destroys a session object.  All allocations are tied to the session will
    be destroyed.

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_NO_MEMORY
*/
NV_STATUS nvUvmInterfaceSessionDestroy(uvmGpuSessionHandle session);

/*******************************************************************************
    nvUvmInterfaceAddressSpaceCreate

    This function creates an address space.
    This virtual address space is created on the GPU specified
    by the gpuUuid.


    Error codes:
      NV_ERR_GENERIC
      NV_ERR_NO_MEMORY
*/
NV_STATUS nvUvmInterfaceAddressSpaceCreate(uvmGpuSessionHandle session,
                                            NvProcessorUuid *gpuUuid,
                                            uvmGpuAddressSpaceHandle *vaSpace,
                                            unsigned long long vaBase,
                                            unsigned long long vaSize);

/*******************************************************************************
    nvUvmInterfaceDupAddressSpace

    This function will dup the given vaspace from the users client to the 
    kernel client was created as an ops session.
    
    By duping the vaspace it is guaranteed that RM will refcount the vaspace object.

    Error codes:
      NV_ERR_GENERIC
*/
NV_STATUS nvUvmInterfaceDupAddressSpace(uvmGpuSessionHandle session,
                                        NvU8 *pUuid,
                                        NvHandle hUserClient,
                                        NvHandle hUserVASpace,
                                        uvmGpuAddressSpaceHandle *vaSpace);

/*******************************************************************************
    nvUvmInterfaceAddressSpaceCreateMirrored

    This function will associate a privileged address space which mirrors the
    address space associated to the provided PID.

    This address space will have a 128MB carveout. All allocations will
    automatically be limited to this carve out.

    This function will be meaningful and needed only for Kepler.

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_NO_MEMORY
*/
NV_STATUS nvUvmInterfaceAddressSpaceCreateMirrored(uvmGpuSessionHandle session,
                                             NvProcessorUuid *gpuUuid,
                                             uvmGpuAddressSpaceHandle *vaSpace);

/*******************************************************************************
    nvUvmInterfaceAddressSpaceDestroy

    Destroys an address space that was previously created via
    nvUvmInterfaceAddressSpaceCreate or
    nvUvmInterfaceAddressSpaceCreateMirrored.
*/

void nvUvmInterfaceAddressSpaceDestroy(uvmGpuAddressSpaceHandle vaSpace);

/*******************************************************************************
    nvUvmInterfaceMemoryAllocFB

    This function will allocate video memory and provide a mapped Gpu
    virtual address to this allocation. It also returns the Gpu physical
    offset if contiguous allocations are requested.

    This function will allocate a minimum page size if the length provided is 0
    and will return a unique GPU virtual address.

    The default page size will be the small page size (as returned by query
    caps). The Alignment will also be enforced to small page size(64K/128K).
 
    Arguments:
        vaSpace[IN]          - Pointer to vaSpace object
        length [IN]          - Length of the allocation
        gpuPointer[OUT]      - GPU VA mapping
        allocInfo[IN/OUT]    - Pointer to allocation info structure which
                               contains below given fields
 
        allocInfo Members: 
        rangeBegin[IN]             - Allocation will be made between rangeBegin
        rangeEnd[IN]                 and rangeEnd(both inclusive). Default will be
                                     no-range limitation.
        gpuPhysOffset[OUT]         - Physical offset of allocation returned only
                                     if contiguous allocation is requested. 
        bContiguousPhysAlloc[IN]   - Flag to request contiguous allocation. Default
                                     will follow the vidHeapControl default policy.
        bHandleProvided [IN]       - Flag to signify that the client has provided 
                                     the handle for phys allocation.
        hPhysHandle[IN/OUT]        - The handle will be used in allocation if provided.
                                     If not provided; allocator will return the handle
                                     it used eventually.
    Error codes:
        NV_ERR_INVALID_ARGUMENT  
        NV_ERR_NO_MEMORY              - Not enough physical memory to service
                                        allocation request with provided constraints
        NV_ERR_INSUFFICIENT_RESOURCES - Not enough available resources to satisfy allocation request
        NV_ERR_INVALID_OWNER          - Target memory not accessible by specified owner
        NV_ERR_NOT_SUPPORTED          - Operation not supported on broken FB
 
*/
NV_STATUS nvUvmInterfaceMemoryAllocFB(uvmGpuAddressSpaceHandle vaSpace,
                                      NvLength length,
                                      UvmGpuPointer * gpuPointer,
                                      UvmGpuAllocInfo * allocInfo);

/*******************************************************************************
    nvUvmInterfaceMemoryAllocGpuPa

    This function will *ONLY* allocate contiguous physical video memory. 
    There is no mapping provided to the physical memory allocated.
    This is primarily used for testing till the MM module comes up. This will be 
    the API used to allocate physical VIDMEM that the UVM driver will manage for
    cuda applications.

    
    Arguments:
        vaSpace[IN]          - Pointer to vaSpace object
        length [IN]          - Length of the allocation
        gpuPointer[OUT]      - GPU FB offset
        allocInfo[IN/OUT]    - Pointer to allocation info structure which
                               contains below given fields
 
        allocInfo Members: 
        rangeBegin[IN]             - Allocation will be made between rangeBegin
        rangeEnd[IN]                 and rangeEnd(both inclusive). Default will be
                                     no-range limitation.
        gpuPhysOffset[OUT]         - Physical offset of allocation returned only
                                     if contiguous allocation is requested. 
        bContiguousPhysAlloc[IN]   - Flag to request contiguous allocation. Default
                                     will follow the vidHeapControl default policy.
        bHandleProvided [IN]       - Flag to signify that the client has provided 
                                     the handle for phys allocation.
        hPhysHandle[IN/OUT]        - The handle will be used in allocation if provided.
                                     If not provided; allocator will return the handle
                                     it used eventually.
    Error codes:
        NV_ERR_INVALID_ARGUMENT  
        NV_ERR_NO_MEMORY              - Not enough physical memory to service
                                        allocation request with provided constraints
        NV_ERR_INSUFFICIENT_RESOURCES - Not enough available resources to satisfy allocation request
        NV_ERR_INVALID_OWNER          - Target memory not accessible by specified owner
        NV_ERR_NOT_SUPPORTED          - Operation not supported on broken FB
 
*/
NV_STATUS nvUvmInterfaceMemoryAllocGpuPa(uvmGpuAddressSpaceHandle vaSpace,
                                         NvLength length,
                                         UvmGpuPointer * gpuPointer,
                                         UvmGpuAllocInfo * allocInfo);
/*******************************************************************************
    nvUvmInterfaceMemoryAllocSys

    This function will allocate system memory and provide a mapped Gpu
    virtual address to this allocation.

    This function will allocate a minimum page size if the length provided is 0
    and will return a unique GPU virtual address.

    The default page size will be the small page size (as returned by query caps)
    The Alignment will also be enforced to small page size.

    Arguments:
        vaSpace[IN]          - Pointer to vaSpace object
        length [IN]          - Length of the allocation
        gpuPointer[OUT]      - GPU VA mapping
        allocInfo[IN/OUT]    - Pointer to allocation info structure which
                               contains below given fields
 
        allocInfo Members: 
        rangeBegin[IN]             - Allocation will be made between rangeBegin
        rangeEnd[IN]                 and rangeEnd(both inclusive). Default will be
                                     no-range limitation.
        gpuPhysOffset[OUT]         - Physical offset of allocation returned only
                                     if contiguous allocation is requested. 
        bContiguousPhysAlloc[IN]   - Flag to request contiguous allocation. Default
                                     will follow the vidHeapControl default policy.
        bHandleProvided [IN]       - Flag to signify that the client has provided 
                                     the handle for phys allocation.
        hPhysHandle[IN/OUT]        - The handle will be used in allocation if provided.
                                     If not provided; allocator will return the handle
                                     it used eventually.
    Error codes:
        NV_ERR_INVALID_ARGUMENT  
        NV_ERR_NO_MEMORY              - Not enough physical memory to service
                                        allocation request with provided constraints
        NV_ERR_INSUFFICIENT_RESOURCES - Not enough available resources to satisfy allocation request
        NV_ERR_INVALID_OWNER          - Target memory not accessible by specified owner
        NV_ERR_NOT_SUPPORTED          - Operation not supported
*/
NV_STATUS nvUvmInterfaceMemoryAllocSys(uvmGpuAddressSpaceHandle vaSpace,
                                       NvLength length,
                                       UvmGpuPointer * gpuPointer,
                                       UvmGpuAllocInfo * allocInfo);

/*******************************************************************************
    This interface is obsolete. It will be nuked soon.
    Use nvUvmInterfaceGetExternalAllocPtes instead.

    nvUvmInterfaceGetSurfaceMapInfo

    This function return the mapping info of a given allocation.

    Given the handle it will provide a pte template with pte mapping information for the
    input surface. This API will also return the list of pfns to map given the offset and length
    of the surface.

    Arguments:
        vaSpace[IN]            - Pointer to vaSpace object
        hAllocation[IN]        - Handle to the allocation
        surfaceMappingInfo[OUT]- Pointer to the surface mapping attributes
 
        surfaceMappingInfo members: 
        hSourceClient[IN]          - Handle of the original client who owns this surface
        hSourceMemory[IN]          - Original allocation handle of the surface we want mapping info for
        mappingOffset[IN]          - Offset into the surface that needs to be mapped
        mappingLength[IN]          - Length of the mapping
        pageCount[IN/OUT]          - Input will be the number of 4k pfns allocated for the
                                     outout pteArray. Output will be the number of entries 
                                     actually copied. If it is one then it is contiguous.
        *pteTemplate[OUT]          - Template PTE with mapping attributes pre filled in
                                     for this surface
        numPages[OUT]   -          - Num pages to be mapped (of pageSize)
        pteArray[OUT]              - Array of PFNs. Will return pfn's in 4k granularity.
        pageSize[OUT]              - Page size of this allocation
                                     
    Error codes:
        NV_ERR_INVALID_ARGUMENT  
        NV_ERR_NO_MEMORY              - Not enough physical memory to service
                                        allocation request with provided constraints
        NV_ERR_INSUFFICIENT_RESOURCES - Not enough available resources to satisfy allocation request
        NV_ERR_INVALID_OWNER          - Target memory not accessible by specified owner
        NV_ERR_NOT_SUPPORTED          - Operation not supported
*/
NV_STATUS nvUvmInterfaceGetSurfaceMapInfo(uvmGpuAddressSpaceHandle vaSpace,
                                          UvmGpuSurfaceMappingInfo *surfaceMapInfo);


/*******************************************************************************
    nvUvmInterfaceGetP2PCaps

    TODO description

    Arguments:
        p2pCapsParams members:
        pUuids[IN]                - pair of gpu uuids
        peerIds[OUT]              - peer ids between given pair of gpus
        writeSupported[OUT]       - p2p writes between gpus are supported
        readSupported[OUT]        - p2p reads between gpus are supported 
        propSupported[OUT]        - p2p PROP between gpus is supposted 
        nvlinkSupported[OUT]      - nvlink connection between master and slave is supported
        atomicSupported[OUT]      - p2p atomics between gpus are supported
                                     
    Error codes:
        NV_ERR_INVALID_ARGUMENT  
        NV_ERR_GENERIC:
          Unexpected error. We try hard to avoid returning this error
          code,because it is not very informative.
*/
NV_STATUS nvUvmInterfaceGetP2PCaps(UvmGpuP2PCapsParams * p2pCapsParams);

/*******************************************************************************
    nvUvmInterfaceGetPmaObject

    This function will returns pointer to PMA object for the GPU whose UUID is 
    passed as an argument. This PMA object handle is required for invoking PMA 
    for allocate and free pages.

    Arguments:
        uuidMsb [IN]        - MSB part of the GPU UUID.
        uuidLsb [IN]        - LSB part of the GPU UUID.
        pPma [OUT]          - Pointer to PMA object

    Error codes:
        NV_ERR_NOT_SUPPORTED          - Operation not supported on broken FB
        NV_ERR_GENERIC:
          Unexpected error. We try hard to avoid returning this error
          code,because it is not very informative.
*/
NV_STATUS nvUvmInterfaceGetPmaObject(NvProcessorUuid * gpuUUID, void **pPma);

// Mirrors pmaEvictPagesCb_t, see its documentation in pma.h.
typedef NV_STATUS (*uvmPmaEvictPagesCallback)(void *callbackData,
                                              NvU32 pageSize,
                                              NvU64 *pPages,
                                              NvU32 count,
                                              NvU64 physBegin,
                                              NvU64 physEnd);

// Mirrors pmaEvictRangeCb_t, see its documentation in pma.h.
typedef NV_STATUS (*uvmPmaEvictRangeCallback)(void *callbackData, NvU64 physBegin, NvU64 physEnd);

/*******************************************************************************
    nvUvmInterfacePmaRegisterEvictionCallbacks

    Simple wrapper for pmaRegisterEvictionCb(), see its documentation in pma.h.
*/
NV_STATUS nvUvmInterfacePmaRegisterEvictionCallbacks(void *pPma,
                                                     uvmPmaEvictPagesCallback evictPages,
                                                     uvmPmaEvictRangeCallback evictRange,
                                                     void *callbackData);

/******************************************************************************
    nvUvmInterfacePmaUnregisterEvictionCallbacks

    Simple wrapper for pmaUnregisterEvictionCb(), see its documentation in pma.h.
*/
void nvUvmInterfacePmaUnregisterEvictionCallbacks(void *pPma);

/*******************************************************************************
    nvUvmInterfacePmaAllocPages

    @brief Synchronous API for allocating pages from the PMA.
    PMA will decide which pma regions to allocate from based on the provided
    flags.  PMA will also initiate UVM evictions to make room for this
    allocation unless prohibited by PMA_FLAGS_DONT_EVICT.  UVM callers must pass
    this flag to avoid deadlock.  Only UVM may allocated unpinned memory from
    this API.

    For broadcast methods, PMA will guarantee the same physical frames are
    allocated on multiple GPUs, specified by the PMA objects passed in.

    If allocation is contiguous, only one page in pPages will be filled.
    Also, contiguous flag must be passed later to nvUvmInterfacePmaFreePages.

    Arguments:
        pPma[IN]             - Pointer to PMA object
        pageCount [IN]       - Number of pages required to be allocated.
        pageSize [IN]        - 64kb, 128kb or 2mb.  No other values are permissible.
        pPmaAllocOptions[IN] - Pointer to PMA allocation info structure.
        pPages[OUT]          - Array of pointers, containing the PA base 
                               address of each page.

    Error codes:
        NV_ERR_NO_MEMORY:
          Internal memory allocation failed.
        NV_ERR_GENERIC:
          Unexpected error. We try hard to avoid returning this error
          code,because it is not very informative.
*/
NV_STATUS nvUvmInterfacePmaAllocPages(void *pPma,
                                      NvLength pageCount,
                                      NvU32 pageSize,
                                      UvmPmaAllocationOptions *pPmaAllocOptions,
                                      NvU64 *pPages);

/*******************************************************************************
    nvUvmInterfacePmaPinPages

    This function will pin the physical memory allocated using PMA. The pages 
    passed as input must be unpinned else this function will return an error and
    rollback any change if any page is not previously marked "unpinned".

    Arguments:
        pPma[IN]             - Pointer to PMA object.
        pPages[IN]           - Array of pointers, containing the PA base 
                               address of each page to be pinned.
        pageCount [IN]       - Number of pages required to be pinned.
        pageSize [IN]        - Page size of each page to be pinned.
        flags [IN]           - UVM_PMA_CALLED_FROM_PMA_EVICTION if called from
                               PMA eviction, 0 otherwise.
    Error codes:
        NV_ERR_INVALID_ARGUMENT       - Invalid input arguments.
        NV_ERR_GENERIC                - Unexpected error. We try hard to avoid 
                                        returning this error code as is not very
                                        informative.
        NV_ERR_NOT_SUPPORTED          - Operation not supported on broken FB
*/
NV_STATUS nvUvmInterfacePmaPinPages(void *pPma,
                                    NvU64 *pPages,
                                    NvLength pageCount,
                                    NvU32 pageSize,
                                    NvU32 flags);

/*******************************************************************************
    nvUvmInterfacePmaUnpinPages

    This function will unpin the physical memory allocated using PMA. The pages 
    passed as input must be already pinned, else this function will return an 
    error and rollback any change if any page is not previously marked "pinned".
    Behaviour is undefined if any blacklisted pages are unpinned.

    Arguments:
        pPma[IN]             - Pointer to PMA object.
        pPages[IN]           - Array of pointers, containing the PA base 
                               address of each page to be unpinned.
        pageCount [IN]       - Number of pages required to be unpinned.
        pageSize [IN]        - Page size of each page to be unpinned.

    Error codes:
        NV_ERR_INVALID_ARGUMENT       - Invalid input arguments.
        NV_ERR_GENERIC                - Unexpected error. We try hard to avoid 
                                        returning this error code as is not very
                                        informative.
        NV_ERR_NOT_SUPPORTED          - Operation not supported on broken FB
*/
NV_STATUS nvUvmInterfacePmaUnpinPages(void *pPma,
                                      NvU64 *pPages,
                                      NvLength pageCount,
                                      NvU32 pageSize);

/*******************************************************************************
    nvUvmInterfaceMemoryFree

    Free up a GPU allocation
*/

void nvUvmInterfaceMemoryFree(uvmGpuAddressSpaceHandle vaSpace,
                              UvmGpuPointer gpuPointer);

/*******************************************************************************
    nvUvmInterfaceMemoryFreePa

    Free up a GPU PA allocation
*/

void nvUvmInterfaceMemoryFreePa(uvmGpuAddressSpaceHandle vaSpace,
                              UvmGpuPointer gpuPointer);

/*******************************************************************************
    nvUvmInterfacePmaFreePages

    This function will free physical memory allocated using PMA.  It marks a list
    of pages as free. This operation is also used by RM to mark pages as "scrubbed"
    for the initial ECC sweep. This function does not fail.

    When allocation was contiguous, an appropriate flag needs to be passed.

    Arguments:
        pPma[IN]             - Pointer to PMA object
        pPages[IN]           - Array of pointers, containing the PA base 
                               address of each page.
        pageCount [IN]       - Number of pages required to be allocated.
        pageSize [IN]        - Page size of each page
        flags [IN]           - Flags with information about allocation type
                               with the same meaning as flags in options for
                               nvUvmInterfacePmaAllocPages. When called from PMA
                               eviction, UVM_PMA_CALLED_FROM_PMA_EVICTION needs
                               to be added to flags.
    Error codes:
        NV_ERR_INVALID_ARGUMENT  
        NV_ERR_NO_MEMORY              - Not enough physical memory to service
                                        allocation request with provided constraints
        NV_ERR_INSUFFICIENT_RESOURCES - Not enough available resources to satisfy allocation request
        NV_ERR_INVALID_OWNER          - Target memory not accessible by specified owner
        NV_ERR_NOT_SUPPORTED          - Operation not supported on broken FB
*/
void nvUvmInterfacePmaFreePages(void *pPma,
                                NvU64 *pPages,
                                NvLength pageCount,
                                NvU32 pageSize,
                                NvU32 flags);

/*******************************************************************************
    nvUvmInterfaceMemoryCpuMap

    This function creates a CPU mapping to the provided GPU address.
    If the address is not the same as what is returned by the Alloc
    function, then the function will map it from the address provided.
    This offset will be relative to the gpu offset obtained from the
    memory alloc functions.

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_NO_MEMORY
*/
NV_STATUS nvUvmInterfaceMemoryCpuMap(uvmGpuAddressSpaceHandle vaSpace,
                                     UvmGpuPointer gpuPointer,
                                     NvLength length, void **cpuPtr,
                                     NvU32 pageSize);

/*******************************************************************************
    uvmGpuMemoryCpuUnmap

    Unmaps the cpuPtr provided from the process virtual address space.
*/
void nvUvmInterfaceMemoryCpuUnMap(uvmGpuAddressSpaceHandle vaSpace,
                                  void *cpuPtr);

/*******************************************************************************
    nvUvmInterfaceChannelAllocate

    This function will allocate a channel

    UvmGpuChannelPointers: this structure will be filled out with channel
    get/put. The errorNotifier is filled out when the channel hits an RC error.

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_NO_MEMORY
*/

NV_STATUS nvUvmInterfaceChannelAllocate(uvmGpuAddressSpaceHandle  vaSpace,
                                        uvmGpuChannelHandle *channel,
                                        UvmGpuChannelPointers * pointers);

void nvUvmInterfaceChannelDestroy(uvmGpuChannelHandle channel);



/*******************************************************************************
    nvUvmInterfaceChannelTranslateError

    Translates NvNotification::info32 to string
*/

const char* nvUvmInterfaceChannelTranslateError(unsigned info32);

/*******************************************************************************
    nvUvmInterfaceCopyEngineAllocate

    This API is deprecated, nvUvmInterfaceCopyEngineAlloc() should be used
    instead. Removal tracked in http://nvbugs/1734807

    ceIndex should correspond to three possible indexes. 1,2 or N and
    this corresponds to the copy engines available on the gpu.
    ceIndex equal to 0 will return UVM_INVALID_ARGUMENTS.
    If a non existant CE index is used, then this API will fail.

    The copyEngineClassNumber is returned so that the user can
    find the right methods to use on his engine.

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_NO_MEMORY
      UVM_INVALID_ARGUMENTS
*/
NV_STATUS nvUvmInterfaceCopyEngineAllocate(uvmGpuChannelHandle channel,
                                           unsigned indexStartingAtOne,
                                           unsigned * copyEngineClassNumber,
                                           uvmGpuCopyEngineHandle *copyEngine);

/*******************************************************************************
    nvUvmInterfaceCopyEngineAlloc

    copyEngineIndex corresponds to the indexing of the
    UvmGpuCaps::copyEngineCaps array. The possible values are
    [0, UVM_COPY_ENGINE_COUNT_MAX), but notably only the copy engines that have
    UvmGpuCopyEngineCaps::supported set to true can be allocated.

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_NO_MEMORY
*/
NV_STATUS nvUvmInterfaceCopyEngineAlloc(uvmGpuChannelHandle channel,
                                        unsigned copyEngineIndex,
                                        uvmGpuCopyEngineHandle *copyEngine);

/*******************************************************************************
    nvUvmInterfaceQueryCaps

    Return capabilities for the provided GPU.
    If GPU does not exist, an error will be returned.

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_NO_MEMORY
*/
NV_STATUS nvUvmInterfaceQueryCaps(uvmGpuAddressSpaceHandle vaSpace,
                                  UvmGpuCaps * caps);
/*******************************************************************************
    nvUvmInterfaceGetAttachedUuids

    Return 1. a list of UUIDS for all GPUs found.
           2. number of GPUs found.

    Error codes:
      NV_ERR_GENERIC
 */
NV_STATUS nvUvmInterfaceGetAttachedUuids(NvU8 *pUuidList, unsigned *numGpus);

/*******************************************************************************
    nvUvmInterfaceGetGpuInfo

    Return various gpu info, refer to the UvmGpuInfo struct for details.
    If no gpu matching the uuid is found, an error will be returned.

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_INSUFFICIENT_RESOURCES
 */
NV_STATUS nvUvmInterfaceGetGpuInfo(NvProcessorUuid *gpuUuid, UvmGpuInfo *pGpuInfo);

/*******************************************************************************
    nvUvmInterfaceGetUvmPrivRegion

    Return UVM privilege region start and length
 */
NV_STATUS nvUvmInterfaceGetUvmPrivRegion(NvU64 *pUvmPrivRegionStart,
                                         NvU64 *pUvmPrivRegionLength);

/*******************************************************************************
    nvUvmInterfaceServiceDeviceInterruptsRM

    Tells RM to service all pending interrupts. This is helpful in ECC error
    conditions when ECC error interrupt is set & error can be determined only
    after ECC notifier will be set or reset.

    Error codes:
      NV_ERR_GENERIC
      UVM_INVALID_ARGUMENTS
*/
NV_STATUS nvUvmInterfaceServiceDeviceInterruptsRM(uvmGpuAddressSpaceHandle vaSpace);


/*******************************************************************************
    nvUvmInterfaceCheckEccErrorSlowpath

    Checks Double-Bit-Error counts thru RM using slow path(Priv-Read) If DBE is
    set in any unit bEccDbeSet will be set to NV_TRUE else NV_FALSE.

    Error codes:
      NV_ERR_GENERIC
      UVM_INVALID_ARGUMENTS
*/
NV_STATUS nvUvmInterfaceCheckEccErrorSlowpath(uvmGpuChannelHandle channel,
                                              NvBool *bEccDbeSet);

/*******************************************************************************
    nvUvmInterfaceKillChannel

    Stops a GPU channel from running, by invoking RC recovery on the channel.

    Error codes:
      NV_ERR_GENERIC
      UVM_INVALID_ARGUMENTS
*/
NV_STATUS nvUvmInterfaceKillChannel(uvmGpuChannelHandle channel);

/*******************************************************************************
    nvUvmInterfaceSetPageDirectory
    Sets pageDirectory in the provided location. Also moves the existing PDE to
    the provided pageDirectory.

    RM will propagate the update to all channels using the provided VA space.
    All channels must be idle when this call is made.

    Arguments:
      vaSpace[IN}         - VASpace Object
      physAddress[IN]     - Physical address of new page directory
      numEntries[IN]      - Number of entries including previous PDE which will be copied
      bVidMemAperture[IN] - If set pageDirectory will reside in VidMem aperture else sysmem

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceSetPageDirectory(uvmGpuAddressSpaceHandle vaSpace,
                                         NvU64 physAddress, unsigned numEntries,
                                         NvBool bVidMemAperture);

/*******************************************************************************
    nvUvmInterfaceUnsetPageDirectory
    Unsets/Restores pageDirectory to RM's defined location.

    Arguments:
      vaSpace[IN}         - VASpace Object

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceUnsetPageDirectory(uvmGpuAddressSpaceHandle vaSpace);

/*******************************************************************************
    nvUvmInterfaceGetGmmuFmt

    Gets GMMU Page Table format.
 
    Arguments:
        vaSpace[IN]          - Pointer to vaSpace object
        pFmt   [OUT]         - Reference of targeted MMU FMT.
 
    Error codes:
      NV_ERR_GENERIC
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceGetGmmuFmt(uvmGpuAddressSpaceHandle vaSpace,
                                   void **pFmt);

/*******************************************************************************
    nvUvmInterfaceDupAllocation

    Duplicate an allocation represented by a physical handle.
    Duplication means: the physical handle will be duplicated from src vaspace 
    to dst vaspace and a new mapping will be created in the dst vaspace.
 
    Arguments:
        hPhysHandle[IN]          - Handle representing the phys allocation.
        srcVaspace[IN]           - Pointer to source vaSpace object
        srcAddress[IN]           - Offset of the gpu mapping in source vaspace.
        dstVaspace[IN]           - Pointer to destination vaSpace object
        dstAddress[OUT]          - Offset of the gpu mapping in destination 
                                   vaspace.
        bPhysHandleValid[IN]     - Whether the client has provided the handle
                                   for source allocation.
                                   If True; hPhysHandle will be used.
                                   Else; ops will find out the handle using
                                   srcVaspace and srcAddress

    Error codes:
      NV_ERROR
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceDupAllocation(NvHandle hPhysHandle,
                                      uvmGpuAddressSpaceHandle srcVaspace,
                                      NvU64 srcAddress,
                                      uvmGpuAddressSpaceHandle dstVaspace,
                                      NvU64 *dstAddress,
                                      NvBool bPhysHandleValid);

/*******************************************************************************
    nvUvmInterfaceDupMemory

    Duplicates a physical memory allocation. If requested, provides information
    about the allocation.
 
    Arguments:
        vaSpace[IN]                     - VA space linked to a client and a device under which
                                          the phys memory needs to be duped.
        hClient[IN]                     - Client owning the memory.
        hPhysMemory[IN]                 - Phys memory which is to be duped.
        hDupedHandle[OUT]               - Handle of the duped memory object.
        pGpuMemoryInfo[OUT]             - see nv_uvm_types.h for more information.
                                          This parameter can be NULL. (optional)
    Error codes:
      NV_ERR_INVALID_ARGUMENT   - If the parameter/s is invalid.
      NV_ERR_NOT_SUPPORTED      - If the allocation is not a physical allocation.
      NV_ERR_OBJECT_NOT_FOUND   - If the allocation is not found in under the provided client.
*/
NV_STATUS nvUvmInterfaceDupMemory(uvmGpuAddressSpaceHandle vaSpace,
                                  NvHandle hClient,
                                  NvHandle hPhysMemory,
                                  NvHandle *hDupMemory,
                                  UvmGpuMemoryInfo *pGpuMemoryInfo);

/*******************************************************************************
    nvUvmInterfaceFreeDupedAllocation

    Free the lallocation represented by the physical handle used to create the
    duped allocation.
 
    Arguments:
        vaspace[IN]              - Pointer to source vaSpace object
        hPhysHandle[IN]          - Handle representing the phys allocation.
        
    Error codes:
      NV_ERROR
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceFreeDupedHandle(uvmGpuAddressSpaceHandle vaspace,
                                        NvHandle hPhysHandle);

/*******************************************************************************
    nvUvmInterfaceGetFbInfo

    Gets FB information from RM.
 
    Arguments:
        vaspace[IN]       - Pointer to source vaSpace object
        fbInfo [OUT]      - Pointer to FbInfo structure which contains
                            reservedHeapSize & heapSize
    Error codes:
      NV_ERROR
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceGetFbInfo(uvmGpuAddressSpaceHandle vaSpace,
                                  UvmGpuFbInfo * fbInfo);

/*******************************************************************************
    nvUvmInterfaceGetGpuIds
        Get GPU deviceId and subdeviceId from RM. UVM maintains a global table 
        indexed by (device, subdevice) pair for easy lookup.

    Arguments:
        pUuid[IN]            - gpu uuid
        uuidLength[IN]       - length of gpu uuid
        pDeviceId[OUT]       - device Id used by RM for given gpu
        pSubdeviceId[OUT]    - sub-device Id used by RM for given gpu

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceGetGpuIds(NvU8 *pUuid, unsigned uuidLength,
    NvU32 *pDeviceId, NvU32 *pSubdeviceId);
/*******************************************************************************
    nvUvmInterfaceOwnPageFaultIntr

    This function transfers ownership of the replayable page fault interrupt,
    between RM and UVM, for a particular GPU.

    bOwnInterrupts == NV_TRUE: UVM is taking ownership from the RM. This causes
    the following: RM will not service, enable or disable this interrupt and it
    is up to the UVM driver to handle this interrupt. In this case, replayable
    page fault interrupts are disabled by this function, before it returns.

    bOwnInterrupts == NV_FALSE: UVM is returning ownership to the RM: in this
    case, replayable page fault interrupts MUST BE DISABLED BEFORE CALLING this
    function.

    The cases above both result in transferring ownership of a GPU that has its
    replayable page fault interrupts disabled. Doing otherwise would make it
    very difficult to control which driver handles any interrupts that build up
    during the hand-off.

    The calling pattern should look like this:

    UVM setting up a new GPU for operation:
        UVM GPU LOCK
           nvUvmInterfaceOwnPageFaultIntr(..., NV_TRUE)
        UVM GPU UNLOCK

        Enable replayable page faults for that GPU

    UVM tearing down a GPU:

        Disable replayable page faults for that GPU

        UVM GPU GPU LOCK
           nvUvmInterfaceOwnPageFaultIntr(..., NV_FALSE)
        UVM GPU UNLOCK

    Arguments:
        pUuid[IN]            - UUID of the GPU to operate on
        uuidLength[IN]       - length of GPU UUID
        bOwnInterrupts       - Set to NV_TRUE for UVM to take ownership of the
                               replayable page fault interrupts. Set to NV_FALSE
                               to return ownership of the page fault interrupts
                               to RM.
    Error codes:
      NV_ERR_GENERIC
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceOwnPageFaultIntr(NvU8 *pUuid, unsigned uuidLength,
                                         NvBool bOwnInterrupts);
/*******************************************************************************
    nvUvmInterfaceInitFaultInfo

    This function obtains fault buffer address, size and a few register mappings

    Arguments:
        vaspace[IN]       - Pointer to vaSpace object associated with the gpu
        pFaultInfo[OUT]   - information provided by RM for fault handling

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceInitFaultInfo(uvmGpuAddressSpaceHandle vaSpace,
    UvmGpuFaultInfo *pFaultInfo);

/*******************************************************************************
    nvUvmInterfaceDestroyFaultInfo

    This function obtains destroys unmaps the fault buffer and clears faultInfo

    Arguments:
        vaspace[IN]       - Pointer to vaSpace object associated with the gpu
        pFaultInfo[OUT]   - information provided by RM for fault handling

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceDestroyFaultInfo(uvmGpuAddressSpaceHandle vaSpace,
    UvmGpuFaultInfo *pFaultInfo);

/*******************************************************************************
    nvUvmInterfaceInitAccessCntrInfo

    This function obtains access counter buffer address, size and a few register mappings
 
    Arguments:
        vaspace[IN]       - Pointer to vaSpace object associated with the gpu
        pAccessCntrInfo[OUT]   - information provided by RM for access counter handling

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceInitAccessCntrInfo(uvmGpuAddressSpaceHandle vaSpace,
    UvmGpuAccessCntrInfo *pAccessCntrInfo);

/*******************************************************************************
    nvUvmInterfaceDestroyAccessCntrInfo

    This function obtains, destroys, unmaps the access counter buffer and clears accessCntrInfo
 
    Arguments:
        vaspace[IN]       - Pointer to vaSpace object associated with the gpu
        pAccessCntrInfo[OUT]   - information provided by RM for access counter handling

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceDestroyAccessCntrInfo(uvmGpuAddressSpaceHandle vaSpace,
    UvmGpuAccessCntrInfo *pAccessCntrInfo);

/*******************************************************************************
    nvUvmInterfaceGetPageLevelInfo

    This function obtains the physical properties of a mapping for a given va
    in the vaspace.
 
    Arguments:
        vaspace[IN]             - Pointer to vaSpace object associated with the gpu
        vAddr[IN]               - virtual address
        pPageLevelInfo[OUT]     - struct holding the physical properties for the mapping.

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceGetPageLevelInfo(uvmGpuAddressSpaceHandle vaSpace, NvU64 vAddr,
                                         UvmGpuPageLevelInfo *pPageLevelInfo);

/*******************************************************************************
    nvUvmInterfaceGetChannelPhysInfo

    This function obtains the physical properties of channel represented by a
    handle in the given client on a given gpu.
 
    Arguments:
        hClient[IN]             - Client's handle in the process context
        hChannel[IN]            - channel handle in the process context
        pChannelInfo[OUT]       - struct holding the channel properties

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceGetChannelPhysInfo(NvHandle hClient, NvHandle hChannel,
                                           UvmGpuChannelPhysInfo *pChannelInfo);

/*******************************************************************************
    This API is obsolete. Soon to be nuked.

    nvUvmInterfaceFreeMemHandles

    This function frees the reference on RM memory descriptors

    Arguments:
        memHandleList[IN]   - list of memory handles
        handleCount[IN]     - number of handles to free

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_INVALID_ARGUMENT
*/
void nvUvmInterfaceFreeMemHandles(void** memHandleList, NvU32 handleCount);

/*******************************************************************************
    This API is obsolete. Soon to be nuked.
    nvUvmInterfaceBindChannel

    This function assumes that the channel buffers are mapped to the VA provided 
    as input. This VA is updated in RM data structures and used to bind channel.
 
    Arguments:
        vaSpace[IN]             - Pointer to vaSpace object associated with the gpu
        hUserClient[IN]         - Client's handle in the process context
        hUserChannel[IN]        - Channel handle in the process context
        bufferCount[IN]         - Number of context buffers mapped by UVM
        bufferVaList[IN]        - Struct holding buffer handle and its va mapping

    Error codes:
      NV_ERR_INSUFFICIENT_RESOURCES
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceBindChannel(uvmGpuAddressSpaceHandle vaSpace,
                                    NvHandle hUserClient, NvHandle hUserChannel,
                                    NvU32 bufferCount, UvmGpuChannelBufferVa *bufferVaList);

/*******************************************************************************
    This API is obsolete. Soon to be nuked.
    nvUvmInterfaceGetCtxBufferCount

    This function obtains the maximum number of context buffers allocated for
    provided channel.
 
    Arguments:
        vaSpace[IN]             - Pointer to vaSpace object associated with the gpu
        bufferCount[OUT]        - Number of context buffers allocated in channel

    Error codes:
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceGetCtxBufferCount(uvmGpuAddressSpaceHandle vaSpace,
                                          NvU32 *bufferCount);

/*******************************************************************************
    This API is obsolete. Soon to be nuked.
    nvUvmInterfaceGetCtxBufferInfo

    This function obtains the channel's context buffers properties like size,
    alignment etc. represented by a handle in the given client on a given gpu.
 
    Arguments:
        vaSpace[IN]             - Pointer to vaSpace object associated with the gpu
        hCudaClient[IN]         - Client's handle in the process context
        hChannel[IN]            - Channel handle in the process context
        bufferCount[IN]         - Number of context buffers allocated in channel
        ctxBufferInfo[OUT]      - Struct holding the context buffer properties

    Error codes:
      NV_ERR_INSUFFICIENT_RESOURCES
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceGetCtxBufferInfo(uvmGpuAddressSpaceHandle vaSpace,
                                         NvHandle hCudaClient,
                                         NvHandle hChannel, NvU32 bufferCount,
                                         UvmGpuChannelCtxBufferInfo *ctxBufferInfo);

/*******************************************************************************
    This API is obsolete. Soon to be nuked.
    nvUvmInterfaceGetCtxBufferPhysInfo

    This function obtains the physical addresses of a specific channel context
    buffers represented by a handle in the given client on a given gpu.
 
    Arguments:
        bufferHandle[IN]        - Opaque buffer memDesc handle
        pageCount[IN]           - Number of pages in context buffer allocation
        physAddrArray[OUT]      - PTE array containing physical adddress

    Error codes:
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceGetCtxBufferPhysInfo(void * bufferHandle, NvU64 pageCount,
                                             NvU64 *physAddrArray);

/*******************************************************************************
    This API is obsolete. Soon to be nuked.
    nvUvmInterfaceValidateChannel

    This function validate that the client channel belongs to the correct vaSpace
    and is a valid channel entry within the vaSpace.
 
    Arguments:
        dupedVaSpace[IN]       - Duped VASpace on which CUDA's VaSpace was duped
        hUserVaSpace [IN]      - User's original vaSpace handle
        hUserClient[IN]        - User's client handle in the process context
        hChannel[IN]           - User's channel handle in the process context

    Error codes:
      NV_ERR_GENERIC
      NV_ERR_INVALID_ARGUMENT
*/
NV_STATUS nvUvmInterfaceValidateChannel(uvmGpuAddressSpaceHandle dupedVaSpace,
                                        NvHandle hUserVaSpace,
                                        NvHandle hUserClient,
                                        NvHandle hUserChannel);

//
// Called by the UVM driver to register operations with RM. Only one set of
// callbacks can be registered by any driver at a time. If another set of
// callbacks was already registered, NV_ERR_IN_USE is returned.
//
NV_STATUS nvUvmInterfaceRegisterUvmCallbacks(struct UvmOpsUvmEvents *importedUvmOps);

//
// Counterpart to nvUvmInterfaceRegisterUvmCallbacks. This must only be called
// if nvUvmInterfaceRegisterUvmCallbacks returned NV_OK.
//
// Upon return, the caller is guaranteed that any outstanding callbacks are done
// and no new ones will be invoked.
//
void nvUvmInterfaceDeRegisterUvmOps(void);

//
// TODO: Find out if there is an RM call that returns this information.
// Meanwhile we will set this to 2 which is the case for the biggest GPUs:
//
#define MAX_NUM_COPY_ENGINES  2

/*******************************************************************************
    This API is obsolete. Soon to be nuked.
    nvUvmInterfaceStopVaspaceChannels

    This API idles all channel associated with a VA space and takes them off the
    runlist. UVM makes this call before destroying the PDB on the VA SPACE

    Arguments:
        session[IN]         - Session handle created for this gpu
        dupVaSpace[IN]      - Pointer to duped vaSpace object created by UVM
        hUserClient[IN]     - Client's handle in the process context
        hUserVa[IN]         - VA space handle created by the user

    Error codes:
      NV_ERR_INVALID_ARGUMENT
      NV_ERR_INVALID_OBJECT/CLIENT : these error codes are obtained if RM has
      already freed the channel that UVM is trying to idle
*/
NV_STATUS nvUvmInterfaceStopVaspaceChannels(uvmGpuSessionHandle session,
                                           uvmGpuAddressSpaceHandle dupVaSpace,
                                           NvHandle hClient,
                                           NvHandle hUserVa);

/*******************************************************************************
    nvUvmInterfaceP2pObjectCreate

    This API creates an NV50_P2P object for given UUIDs and returns
    the handle to the object.

    Arguments:
        session[IN]         - Session handle.
        uuid1[IN]           - uuid of a GPU.
        uuid2[IN]           - uuid of a GPU.
        hP2pObject[OUT]     - handle to the created P2p object.

    Error codes:
      NV_ERR_INVALID_ARGUMENT
      NV_ERR_OBJECT_NOT_FOUND : If device object associated with the uuids aren't found.
*/
NV_STATUS nvUvmInterfaceP2pObjectCreate(uvmGpuSessionHandle session,
                                        NvProcessorUuid *uuid1,
                                        NvProcessorUuid *uuid2,
                                        NvHandle *hP2pObject);

/*******************************************************************************
    nvUvmInterfaceP2pObjectDestroy

    This API destroys the NV50_P2P associated with the passed handle.

    Arguments:
        session[IN]        - Session handle.
        hP2pObject[IN]     - handle to an P2p object.

    Error codes: NONE
*/
void nvUvmInterfaceP2pObjectDestroy(uvmGpuSessionHandle session,
                                    NvHandle hP2pObject);

/*******************************************************************************
    nvUvmInterfaceGetBigPageSize

    Returns bigPageSize associated with the GPU VA Space.
    If VA Space does not exist, an error will be returned.

    Arguments:
        vaSpace[IN]          - vaSpace handle.
        bigPageSize[OUT]     - big page size associated with the vaSpace.

    Error codes:
        NV_ERR_INVALID_ARGUMENT
        NV_ERR_OBJECT_NOT_FOUND : If objects associated with the handle aren't found.
 */
NV_STATUS nvUvmInterfaceGetBigPageSize(uvmGpuAddressSpaceHandle vaSpace,
                                       NvU32 *bigPageSize);

/*******************************************************************************
    nvUvmInterfaceGetExternalAllocPtes

    The interface builds the RM PTEs using the provided input parameters.

    Arguments:
        vaSpace[IN]                     -  vaSpace handle.
        hMemory[IN]                     -  Memory handle.
        offset [IN]                     -  Offset from the beginning of the allocation
                                           where PTE mappings should begin.
                                           Should be aligned with pagesize associated
                                           with the allocation.
        size [IN]                       -  Length of the allocation for which PTEs
                                           should be built.
                                           Should be aligned with pagesize associated
                                           with the allocation.
                                           size = 0 will be interpreted as the total size
                                           of the allocation.
        gpuExternalMappingInfo[IN/OUT]  -  See nv_uvm_types.h for more information.

   Error codes:
        NV_ERR_INVALID_ARGUMENT         - Invalid parameter/s is passed.
        NV_ERR_INVALID_OBJECT_HANDLE    - Invalid memory handle is passed.
        NV_ERR_NOT_SUPPORTED            - Functionality is not supported (see comments in nv_gpu_ops.c)
        NV_ERR_INVALID_BASE             - offset is beyond the allocation size
        NV_ERR_INVALID_LIMIT            - (offset + size) is beyond the allocation size.
        NV_ERR_BUFFER_TOO_SMALL         - gpuExternalMappingInfo.pteBufferSize is insufficient to
                                          store single PTE.
*/
NV_STATUS nvUvmInterfaceGetExternalAllocPtes(uvmGpuAddressSpaceHandle vaSpace,
                                             NvHandle hMemory,
                                             NvU64 offset,
                                             NvU64 size,
                                             UvmGpuExternalMappingInfo *gpuExternalMappingInfo);

/*******************************************************************************
    nvUvmInterfaceRetainChannel

    Returns information about the channel instance associated with the channel.
    Also, it refcounts the channel instance.

    Arguments:
        vaSpace[IN]               - vaSpace handle.
        hClient[IN]               - Client handle
        hChannel[IN]              - Channel handle
        channelInstanceInfo[OUT]  - The channel instance information will be written in this param.
                                      see nv_uvm_types.h for details.

    Error codes:
        NV_ERR_INVALID_ARGUMENT : If the parameter/s are invalid.
        NV_ERR_OBJECT_NOT_FOUND : If the object associated with the handle isn't found.
        NV_ERR_INVALID_CHANNEL : If the channel verification fails.
 */
NV_STATUS nvUvmInterfaceRetainChannel(uvmGpuAddressSpaceHandle vaSpace,
                                      NvHandle hClient,
                                      NvHandle hChannel,
                                      UvmGpuChannelInstanceInfo *channelInstanceInfo);

/*******************************************************************************
    nvUvmInterfaceRetainChannelResources

    Returns information about channel resources (local CTX buffers + global CTX buffers).
    Also, it refcounts the memory descriptors associated with the resources.

    Arguments:
        vaSpace[IN]               - vaSpace handle.
        instanceDescriptor[IN]    - The instance pointer descriptor returned by returned by
                                    nvUvmInterfaceRetainChannelResources.
        resourceCount[IN]         - Number of resources queried using nvUvmInterfaceRetainChannel.
        channelResourceInfo[OUT]  - This should be the buffer of (sizeof(UvmGpuChannelResourceInfo) * resourceCount).
                                      The channel resource information will be written in this buffer.
                                      see nv_uvm_types.h for details.

    Error codes:
        NV_ERR_INVALID_ARGUMENT : If the parameter/s are invalid.
        NV_ERR_OBJECT_NOT_FOUND : If the object associated with the handle isn't found.
        NV_ERR_INSUFFICIENT_RESOURCES : If no memory available to store the resource information.
 */
NV_STATUS nvUvmInterfaceRetainChannelResources(uvmGpuAddressSpaceHandle vaSpace,
                                               NvP64 instanceDescriptor,
                                               NvU32 resourceCount,
                                               UvmGpuChannelResourceInfo *channelResourceInfo);

/*******************************************************************************
    nvUvmInterfaceBindChannelResources

    Associates the mapping address of the channel resources (VAs) provided by the
    caller with the channel.

    Arguments:
        vaSpace[IN]                   - vaSpace handle.
        instanceDescriptor[IN]        - The instance pointer descriptor returned by returned by
                                        nvUvmInterfaceRetainChannelResources.
        resourceCount[IN]             - Number of resources queried using nvUvmInterfaceRetainChannel.
        channelResourceBindParams[IN] - The call expects the buffer of (sizeof(UvmGpuChannelResourceBindParams) *
                                        resourceCount) initialized with the mapping addresses and
                                        resource information as input.
                                        see nv_uvm_types.h for details.

    Error codes:
        NV_ERR_INVALID_ARGUMENT : If the parameter/s are invalid.
        NV_ERR_OBJECT_NOT_FOUND : If the object associated with the handle aren't found.
        NV_ERR_INSUFFICIENT_RESOURCES : If no memory available to store the resource information.
 */
NV_STATUS nvUvmInterfaceBindChannelResources(uvmGpuAddressSpaceHandle vaSpace,
                                             NvP64 instanceDescriptor,
                                             NvU32 resourceCount,
                                             UvmGpuChannelResourceBindParams *channelResourceBindParams);

/*******************************************************************************
    nvUvmInterfaceReleaseChannel

    Release refcounts on the memory descriptor associated with the channel instance.
    Also, frees the memory descriptor if refcount reaches zero.

    Arguments:
        descriptors[IN]         - The descriptor returned by nvUvmInterfaceRetainChannel as input.
 */
void nvUvmInterfaceReleaseChannel(NvP64 instanceDescriptor);

/*******************************************************************************
    nvUvmInterfaceReleaseChannelResources

    Release refcounts on the memory descriptors associated with the resources.
    Also, frees the memory descriptors if refcount reaches zero.

    Arguments:
        descriptors[IN]         - The call expects the input buffer of size(NvP64) * descriptorCount initialized
                                  with the descriptors returned by nvUvmInterfaceRetainChannelResources as input.
        descriptorCount[IN]     - The count of descriptors to be released.
 */
void nvUvmInterfaceReleaseChannelResources(NvP64 *resourceDescriptors, NvU32 descriptorCount);

/*******************************************************************************
    nvUvmInterfaceStopChannel

    Idles the channel and takes it off the runlist.

    Arguments:
        vaSpace[IN]                   - vaSpace handle.
        instanceDescriptor[IN]        - The instance pointer descriptor returned by returned by
                                        nvUvmInterfaceRetainChannelResources.
        bImmediate[IN]                - If true, kill the channel without attempting to wait for it to go idle.
*/
void nvUvmInterfaceStopChannel(uvmGpuAddressSpaceHandle vaSpace, NvP64 instanceDescriptor, NvBool bImmediate);

/*******************************************************************************
    nvUvmInterfaceGetChannelResourcePtes

    The interface builds the RM PTEs using the provided input parameters.

    Arguments:
        vaSpace[IN]                     -  vaSpace handle.
        resourceDescriptor[IN]          -  The channel resource descriptor returned by returned by
                                           nvUvmInterfaceRetainChannelResources.
        offset[IN]                      -  Offset from the beginning of the allocation
                                           where PTE mappings should begin.
                                           Should be aligned with pagesize associated
                                           with the allocation.
        size[IN]                        -  Length of the allocation for which PTEs
                                           should be built.
                                           Should be aligned with pagesize associated
                                           with the allocation.
                                           size = 0 will be interpreted as the total size
                                           of the allocation.
        gpuExternalMappingInfo[IN/OUT]  -  See nv_uvm_types.h for more information.

   Error codes:
        NV_ERR_INVALID_ARGUMENT         - Invalid parameter/s is passed.
        NV_ERR_INVALID_OBJECT_HANDLE    - Invalid memory handle is passed.
        NV_ERR_NOT_SUPPORTED            - Functionality is not supported.
        NV_ERR_INVALID_BASE             - offset is beyond the allocation size
        NV_ERR_INVALID_LIMIT            - (offset + size) is beyond the allocation size.
        NV_ERR_BUFFER_TOO_SMALL         - gpuExternalMappingInfo.pteBufferSize is insufficient to
                                          store single PTE.
*/
NV_STATUS nvUvmInterfaceGetChannelResourcePtes(uvmGpuAddressSpaceHandle vaSpace,
                                               NvP64 resourceDescriptor,
                                               NvU64 offset,
                                               NvU64 size,
                                               UvmGpuExternalMappingInfo *externalMappingInfo);

#endif // _NV_UVM_INTERFACE_H_
