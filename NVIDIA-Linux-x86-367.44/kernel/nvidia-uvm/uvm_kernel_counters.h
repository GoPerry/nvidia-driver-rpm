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

#ifndef UVM_COMMON_COUNTERS_H_
#define UVM_COMMON_COUNTERS_H_

#include "uvm-debug.h"
#include "uvm_common.h"

// size of counters per gpu / aggregate in bytes
static const unsigned UVM_PER_RESOURCE_COUNTERS_SIZE = PAGE_SIZE;
// size of a single counter in bytes
static const unsigned UVM_COUNTER_SIZE = sizeof(unsigned long long *);
// shift for per process per gpu counters in user mapping.
static const int UVM_PER_PROCESS_PER_GPU_COUNTERS_SHIFT = PAGE_SIZE;

//
// UVM Counter memory descriptors and users information
//
typedef struct UvmCounterInfo_tag
{
    // physical page *
    struct page *pCounterPage;

    // kernel mapping of above page
    unsigned long long *sysAddr;

    // Number of enabled sessions for each counter
    atomic_t sessionCount[UVM_TOTAL_COUNTERS];

} UvmCounterInfo;

typedef struct UvmCounterContainer_tag
{
    // indexed according to g_attached_uuid_list
    UvmCounterInfo perGpuCounterArray[UVM_MAX_GPUS];
    UvmCounterInfo allGpuCounter;
    atomic_t refcountUsers;
} UvmCounterContainer;

// API Initialization functions
// Function used to initialize the counter system. It must be called before
// calling any counter or event functions
NV_STATUS uvm_initialize_counters_api(void);
void uvm_deinitialize_counters_api(void);

// Allocation and ref counting functions
NV_STATUS uvm_alloc_counter_container(UvmCounterContainer **ppCounterContainer);
void uvm_ref_counter_container(UvmCounterContainer *pCounterContainer);
void uvm_unref_counter_container(UvmCounterContainer *pCounterContainer);

NV_STATUS uvm_map_counters_pages(UvmCounterContainer *pCounterContainer,
                                 NvP64 userCountersBaseAddr,
                                 struct vm_area_struct *pVma);

void uvm_increment_process_counters(unsigned gpuIndex,
                                    UvmCounterContainer *pCounterContainer,
                                    UvmCounterName name,
                                    unsigned incrementVal);

NV_STATUS uvm_get_counter_index(UvmCounterName counterName,
                                unsigned *counterIndex);

#endif // UVM_COMMON_COUNTERS_H_
