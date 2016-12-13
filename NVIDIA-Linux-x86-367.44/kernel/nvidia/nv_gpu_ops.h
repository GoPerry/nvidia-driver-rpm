/* _NVRM_COPYRIGHT_BEGIN_
 *
 * Copyright 2013-2015 by NVIDIA Corporation.  All rights reserved.  All
 * information contained herein is proprietary and confidential to NVIDIA
 * Corporation.  Any use, reproduction, or disclosure without the written
 * permission of NVIDIA Corporation is prohibited.
 *
 * _NVRM_COPYRIGHT_END_
 */


/*
 * nv_gpu_ops.h
 *
 * This file defines the interface between the common RM layer
 * and the OS specific platform layers. (Currently supported 
 * are Linux and KMD)
 *
 */

#ifndef _NV_GPU_OPS_H_
#define _NV_GPU_OPS_H_
#include "nvgputypes.h"
#include "nv_uvm_types.h"

//
// Default Page Size if left "0" because in RM BIG page size is default & there
// are multiple BIG page sizes in RM. These defines are used as flags to "0" 
// should be OK when user is not sure which pagesize allocation it wants
//
#define PAGE_SIZE_DEFAULT    0x0
#define PAGE_SIZE_4K         0x1000
#define PAGE_SIZE_64K        0x10000
#define PAGE_SIZE_128K       0x20000
#define PAGE_SIZE_2M         0x200000

#define GPU_PAGE_LEVEL_INFO_MAX_LEVELS         5
#define GPU_PAGE_LEVEL_INFO_LEVEL_4K           0
#define GPU_PAGE_LEVEL_INFO_LEVEL_64K          1
#define GPU_PAGE_LEVEL_INFO_LEVEL_2M           2

typedef struct gpuSession      *gpuSessionHandle;
typedef struct gpuAddressSpace *gpuAddressSpaceHandle;
typedef struct gpuChannel      *gpuChannelHandle;
typedef struct gpuObject       *gpuObjectHandle;
typedef struct gpuChannelCtxBufferInfo * gpuChannelCtxBufferInfoHandle;
typedef struct gpuChannelBufferVa   *gpuChannelBufferVaHandle;

typedef UvmGpuChannelPointers gpuChannelInfo;
typedef UvmGpuCaps gpuCaps;
typedef UvmGpuP2PCapsParams getP2PCapsParams;
typedef UvmGpuAllocInfo gpuAllocInfo;
typedef UvmGpuInfo gpuInfo;
typedef UvmGpuAccessCntrInfo gpuAccessCntrInfo;
typedef UvmGpuFaultInfo gpuFaultInfo;
typedef UvmGpuMemoryInfo gpuMemoryInfo;
typedef UvmGpuExternalMappingInfo gpuExternalMappingInfo;
typedef UvmGpuChannelResourceInfo gpuChannelResourceInfo;
typedef UvmGpuChannelInstanceInfo gpuChannelInstanceInfo;
typedef UvmGpuChannelResourceBindParams gpuChannelResourceBindParams;
typedef UvmGpuFbInfo gpuFbInfo;

// Note: Do not modify these structs as nv_uvm_types.h holds
// another definition of the same. Modifying one of the copies will lead
// to struct member mismatch. Note, compiler does not catch such errors.
// TODO: Nuke these structs and add a typedef for UvmGpuTypes*.
struct gpuVaAllocInfo
{
    NvU64    vaStart;                    // Needs to be alinged to pagesize
    NvBool   bFixedAddressAllocate;      // rangeBegin & rangeEnd both included
    NvU32    pageSize;                   // default is where both 4k and 64k page tables will be allocated. 
};

struct gpuMapInfo
{
   NvBool      bPteFlagReadOnly;
   NvBool      bPteFlagAtomic;
   NvBool      bPteFlagsValid;
   NvBool      bApertureIsVid;
   NvBool      bIsContiguous;
   NvU32       pageSize;
};

struct gpuPmaAllocationOptions
{
    NvU32 flags;
    NvU32 minimumSpeed;         // valid if flags & PMA_ALLOCATE_SPECIFY_MININUM_SPEED
    NvU64 physBegin, physEnd;   // valid if flags & PMA_ALLOCATE_SPECIFY_ADDRESS_RANGE
    NvU32 regionId;             // valid if flags & PMA_ALLOCATE_SPECIFY_REGION_ID
};

struct gpuPageLevelInfo
{
    NvU32 pageSize;
    NvU64 vAddr;
    struct
    {
        NvU64 physAddress;
        NvU32 aperture;
    } levels[GPU_PAGE_LEVEL_INFO_MAX_LEVELS];
};

struct gpuChannelPhysInfo
{
    NvU64 pdb;
    NvBool bPdbLocVidmem;
    NvU64 instPtr;
    NvBool bInstPtrLocVidmem;
    NvP64 memHandle; // RM memDesc handle to inst ptr
};

struct gpuSurfaceMappingInfo
{
    NvHandle hSourceClient;
    NvHandle hSourceMemory;
    NvU64 mappingOffset;
    NvU64 mappingLength;
    NvU8  *pteTemplate;
    NvU64 pageCount;
    NvU64 *pteArray;
    NvU32 pageSize;
};

struct gpuChannelCtxBufferInfo
{
    NvU64   alignment;      // Buffer aligment
    NvU64   size;           // Buffer allocation size after enforcing alignment
    NvP64   bufferHandle;   // RM memDesc handle for buffer
    NvU64   pageCount;      // Number of pages allocated
    NvU32   aperture;       // Allocation aperture
    NvBool  bIsContigous;   // Set if allocation is physically contigous
    NvBool  bGlobalBuffer;  // Set if global buffer as they are mapped only once
    NvBool  bLocalBuffer;   // Set if local buffer as they are mapped per channel
};

struct gpuChannelBufferVa
{
    // RM memDesc handle to channel buffer
    NvP64 bufferHandle;
    // Virtual address where the RM buffer is mapped
    NvP64 bufferVa;
    NvBool bIsGlobalBuffer;
};

NV_STATUS nvGpuOpsCreateSession(gpuSessionHandle *session);

void nvGpuOpsDestroySession(gpuSessionHandle session);

NV_STATUS nvGpuOpsAddressSpaceCreate(gpuSessionHandle session,
                                     NvProcessorUuid *gpuUuid,
                                     gpuAddressSpaceHandle *vaSpace,
                                     unsigned long long vaBase,
                                     unsigned long long vaSize);

NV_STATUS nvGpuOpsAddressSpaceCreateMirrored(gpuSessionHandle session,
                                     NvProcessorUuid *gpuUuid,
                                     gpuAddressSpaceHandle *vaSpace);
NV_STATUS nvGpuOpsGetSurfaceMapInfo(struct gpuAddressSpace *vaSpace,
                                    struct gpuSurfaceMappingInfo *pSurfacemapInfo);

NV_STATUS nvGpuOpsGetP2PCaps(getP2PCapsParams *p2pCaps);

NV_STATUS nvGpuOpsAddressSpaceDestroy(gpuAddressSpaceHandle vaSpace);

// nvGpuOpsMemoryAllocGpuPa and nvGpuOpsFreePhysical were added to support UVM driver
// when PMA was not ready. These should not be used anymore and will be nuked soon.
NV_STATUS nvGpuOpsMemoryAllocGpuPa (struct gpuAddressSpace * vaSpace,
    NvLength length, NvU64 *gpuOffset, gpuAllocInfo * allocInfo);

void nvGpuOpsFreePhysical(struct gpuAddressSpace * vaSpace, NvU64 paOffset);

NV_STATUS nvGpuOpsMemoryAllocFb (gpuAddressSpaceHandle vaSpace,
    NvLength length, NvU64 *gpuOffset, gpuAllocInfo * allocInfo);

NV_STATUS nvGpuOpsMemoryAllocSys (gpuAddressSpaceHandle vaSpace,
    NvLength length, NvU64 *gpuOffset, gpuAllocInfo * allocInfo);

NV_STATUS nvGpuOpsPmaAllocPages(void *pPma,
                                NvLength pageCount,
                                NvU32 pageSize,
                                struct gpuPmaAllocationOptions *pPmaAllocOptions,
                                NvU64 *pPages);

void nvGpuOpsPmaFreePages(void *pPma,
                          NvU64 *pPages,
                          NvLength pageCount,
                          NvU32 pageSize,
                          NvU32 flags);

NV_STATUS nvGpuOpsPmaPinPages(void *pPma,
                              NvU64 *pPages,
                              NvLength pageCount,
                              NvU32 pageSize);

NV_STATUS nvGpuOpsPmaUnpinPages(void *pPma,
                                NvU64 *pPages,
                                NvLength pageCount,
                                NvU32 pageSize);

NV_STATUS nvGpuOpsChannelAllocate(gpuAddressSpaceHandle vaSpace,
    gpuChannelHandle  *channelHandle, gpuChannelInfo *channelInfo);

NV_STATUS nvGpuOpsMemoryReopen(struct gpuAddressSpace *vaSpace,
     NvHandle hSrcClient, NvHandle hSrcAllocation, NvLength length, NvU64 *gpuOffset);

void nvGpuOpsChannelDestroy(struct gpuChannel *channel);

void nvGpuOpsMemoryFree(gpuAddressSpaceHandle vaSpace,
     NvU64 pointer);

NV_STATUS  nvGpuOpsMemoryCpuMap(gpuAddressSpaceHandle vaSpace,
                                NvU64 memory, NvLength length,
                                void **cpuPtr, NvU32 pageSize);

void nvGpuOpsMemoryCpuUnMap(gpuAddressSpaceHandle vaSpace,
     void* cpuPtr);

NV_STATUS nvGpuOpsCopyEngineAllocate(gpuChannelHandle channel,
          unsigned ceIndex, unsigned *_class, gpuObjectHandle *copyEngineHandle);

NV_STATUS nvGpuOpsCopyEngineAlloc(gpuChannelHandle channel,
          unsigned ceIndex, gpuObjectHandle *copyEngineHandle);

NV_STATUS nvGpuOpsQueryCaps(struct gpuAddressSpace *vaSpace,
                            gpuCaps *caps);

const char  *nvGpuOpsChannelTranslateError(unsigned info32);

NV_STATUS nvGpuOpsDupAllocation(NvHandle hPhysHandle, 
                                struct gpuAddressSpace *sourceVaspace, 
                                NvU64 sourceAddress,
                                struct gpuAddressSpace *destVaspace,
                                NvU64 *destAddress,
                                NvBool bPhysHandleValid);

NV_STATUS nvGpuOpsDupMemory(struct gpuAddressSpace *vaSpace,
                            NvHandle hClient,
                            NvHandle hPhysMemory,
                            NvHandle *hDupMemory,
                            gpuMemoryInfo *pGpuMemoryInfo);

NV_STATUS nvGpuOpsGetGuid(NvHandle hClient, NvHandle hDevice, 
                          NvHandle hSubDevice, NvU8 *gpuGuid, 
                          unsigned guidLength);
                          
NV_STATUS nvGpuOpsGetClientInfoFromPid(unsigned pid, NvU8 *gpuUuid, 
                                       NvHandle *hClient, 
                                       NvHandle *hDevice,
                                       NvHandle *hSubDevice);

NV_STATUS nvGpuOpsFreeDupedHandle(struct gpuAddressSpace *sourceVaspace,
                                  NvHandle hPhysHandle);

NV_STATUS nvGpuOpsGetAttachedGpus(NvU8 *guidList, unsigned *numGpus);

NV_STATUS nvGpuOpsGetGpuInfo(NvProcessorUuid *gpuUuid, gpuInfo *pGpuInfo);

NV_STATUS nvGpuOpsGetGpuIds(NvU8 *pUuid, unsigned uuidLength, NvU32 *pDeviceId,
                            NvU32 *pSubdeviceId);

NV_STATUS nvGpuOpsOwnPageFaultIntr(NvU8 *pUuid, unsigned uuidLength, 
                                   NvBool bOwnInterrups);

NV_STATUS nvGpuOpsServiceDeviceInterruptsRM(struct gpuAddressSpace *vaSpace);

NV_STATUS nvGpuOpsCheckEccErrorSlowpath(struct gpuChannel * channel, NvBool *bEccDbeSet);

NV_STATUS nvGpuOpsKillChannel(struct gpuChannel * channel);

NV_STATUS nvGpuOpsSetPageDirectory(struct gpuAddressSpace * vaSpace,
                                   NvU64 physAddress, unsigned numEntries,
                                   NvBool bVidMemAperture);

NV_STATUS nvGpuOpsUnsetPageDirectory(struct gpuAddressSpace * vaSpace);

NV_STATUS nvGpuOpsGetGmmuFmt(struct gpuAddressSpace * vaSpace, void ** pFmt);

NV_STATUS nvGpuOpsGetBigPageSize(struct gpuAddressSpace *vaSpace,
                                 NvU32 *bigPageSize);

NV_STATUS nvGpuOpsInvalidateTlb(struct gpuAddressSpace * vaSpace);

NV_STATUS nvGpuOpsGetFbInfo(struct gpuAddressSpace * vaSpace, gpuFbInfo * fbInfo);

NV_STATUS nvGpuOpsInitFaultInfo(struct gpuAddressSpace *vaSpace, gpuFaultInfo *pFaultInfo);

NV_STATUS nvGpuOpsDestroyFaultInfo(struct gpuAddressSpace *vaSpace, gpuFaultInfo *pFaultInfo);

NV_STATUS nvGpuOpsGetPageLevelInfo(struct gpuAddressSpace *vaSpace, NvU64 vAddr, struct gpuPageLevelInfo *pPageLevelInfo);

NV_STATUS nvGpuOpsGetChannelPhysInfo(NvHandle hClient, NvHandle hChannel, struct gpuChannelPhysInfo *pChannelInfo);

void nvGpuOpsFreeMemHandles(void** memHandleList, NvU32 handleCount);

NV_STATUS nvGpuOpsDupAddressSpace(struct gpuSession * session, NvU8 * pUuid, NvHandle hUserClient, NvHandle hUserVASpace, struct gpuAddressSpace **vaSpace);

NV_STATUS nvGpuOpsBindChannel(struct gpuAddressSpace *vaSpace, NvHandle hUserClient, NvHandle hUserChannel, NvU32 bufferCount, struct gpuChannelBufferVa *bufferVaList);

NV_STATUS nvGpuOpsGetCtxBufferCount(struct gpuAddressSpace *vaSpace, NvU32 * bufferCount);

NV_STATUS nvGpuOpsGetCtxBufferInfo(struct gpuAddressSpace *vaSpace, NvHandle hCudaClient,
                                   NvHandle hChannel, NvU32 bufferCount,
                                   struct gpuChannelCtxBufferInfo *ctxBufferInfo);

NV_STATUS nvGpuOpsGetCtxBufferPhysInfo(void * bufferHandle, NvU64 pageCount, NvU64 *physAddrArray);

NV_STATUS nvGpuOpsValidateChannel(struct gpuAddressSpace *dupedVaSpace,
                                  NvHandle hUserVaSpace,
                                  NvHandle hUserClient,
                                  NvHandle hUserChannel);

NV_STATUS nvGpuOpsGetPmaObject(NvProcessorUuid *gpuUUID,
                               void **pPma);

NV_STATUS nvGpuOpsStopVaspaceChannels(struct gpuSession *session,
                                      struct gpuAddressSpace *dpuVaSpace,
                                      NvHandle hClient,
                                      NvHandle hUserVa);

NV_STATUS nvGpuOpsInitAccessCntrInfo(struct gpuAddressSpace *vaSpace, gpuAccessCntrInfo *pAccessCntrInfo);

NV_STATUS nvGpuOpsDestroyAccessCntrInfo(struct gpuAddressSpace *vaSpace, gpuAccessCntrInfo *pAccessCntrInfo);

NV_STATUS nvGpuOpsP2pObjectCreate(struct gpuSession *session,
                                  NvProcessorUuid *gpuUuid1,
                                  NvProcessorUuid *gpuUuid2,
                                  NvHandle *hP2pObject);

void nvGpuOpsP2pObjectDestroy(struct gpuSession *session,
                              NvHandle hP2pObject);

NV_STATUS nvGpuOpsGetExternalAllocPtes(struct gpuAddressSpace *vaSpace,
                                       NvHandle hDupedMemory,
                                       NvU64 offset,
                                       NvU64 size,
                                       gpuExternalMappingInfo *pGpuExternalMappingInfo);

NV_STATUS nvGpuOpsRetainChannel(struct gpuAddressSpace *vaSpace,
                                NvHandle hClient,
                                NvHandle hChannel,
                                gpuChannelInstanceInfo *channelInstanceInfo);

void nvGpuOpsReleaseChannel(NvP64 instanceDescriptor);

NV_STATUS nvGpuOpsRetainChannelResources(struct gpuAddressSpace *vaSpace,
                                         NvP64 instanceDescriptor,
                                         NvU32 resourceCount,
                                         gpuChannelResourceInfo *channelResourceInfo);

NV_STATUS nvGpuOpsBindChannelResources(struct gpuAddressSpace *vaSpace,
                                       NvP64 instanceDescriptor,
                                       NvU32 resourceCount,
                                       gpuChannelResourceBindParams *channelResourceBindParams);

void nvGpuOpsReleaseChannelResources(NvP64 *resourceDescriptors, NvU32 descriptorCount);

void nvGpuOpsStopChannel(struct gpuAddressSpace *vaSpace, NvP64 instanceDescriptor, NvBool bImmediate);

NV_STATUS nvGpuOpsGetChannelResourcePtes(struct gpuAddressSpace *vaSpace,
                                         NvP64 resourceDescriptor,
                                         NvU64 offset,
                                         NvU64 size,
                                         gpuExternalMappingInfo *pGpuExternalMappingInfo);

#endif /* _NV_GPU_OPS_H_*/
