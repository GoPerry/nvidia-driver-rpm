/*******************************************************************************
    Copyright (c) 2014 NVIDIA Corporation

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

//
// uvm_lite_prefetch.h
//
// This file contains declarations for UVM-Lite page prefetching code.
//
//

#ifndef _UVM_LITE_PREFETCH_H
#define _UVM_LITE_PREFETCH_H

#include "uvmtypes.h"
#include "uvm_lite.h"

// Module load/unload functions
NV_STATUS uvmlite_prefetch_init(void);
void uvmlite_prefetch_exit(void);

// Create prefetch information for the given UvmCommitRecord
NV_STATUS
uvmlite_init_prefetch_info(UvmPrefetchInfo *pPrefetchInfo, UvmCommitRecord *pRecord);

// Destroy prefetch information
void
uvmlite_destroy_prefetch_info(UvmPrefetchInfo *pPrefetchInfo);

// Reset prefetch information. Typically used on kernel call boundaries
void
uvmlite_reset_prefetch_info(UvmPrefetchInfo *pPrefetchInfo, UvmCommitRecord *pRecord);

// Request a prefetch command after a major page fault
NvBool
uvmlite_prefetch_log_major_fault(UvmPrefetchInfo *pPrefetchInfo,
                                 UvmCommitRecord *pRecord,
                                 unsigned long pageIndex,
                                 UvmPrefetchHint *hint);

// Notify a minor page fault. Needed to test the accuracy of the prefetcher
void
uvmlite_prefetch_log_minor_fault(UvmPrefetchInfo *pPrefetchInfo, unsigned long pageIndex);

// Notify that the given page has been correctly prefetched
void
uvmlite_prefetch_page_ack(UvmPrefetchInfo *pPrefetchInfo, unsigned long pageIndex);

// Module parameter to enable/disable prefetching in UVM-Lite.
extern int uvm_prefetch;
// Module parameters to tune page prefetching in UVM-Lite
extern int uvm_prefetch_stats;

#endif // _UVM_LITE_PREFETCH_H
