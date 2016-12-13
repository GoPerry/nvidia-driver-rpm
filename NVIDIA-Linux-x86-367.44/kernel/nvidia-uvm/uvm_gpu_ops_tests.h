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
 * This fileContains the call signatures and defines for all RM <-> UVM
 * interface tests.
 */

#ifndef _UVM_GPU_OPS_TESTS_H_
#define _UVM_GPU_OPS_TESTS_H_

#include "uvmtypes.h"
#include "uvm_channel_mgmt.h"
#include "uvm_common_test.h"

#define UVM_RUN_SUBTEST(status, TestFunction, ...)                        \
do                                                                        \
{                                                                         \
    status = TestFunction(__VA_ARGS__);                                   \
    if (NV_OK != status)                                                  \
        UVM_ERR_PRINT_NV_STATUS("FAIL: "#TestFunction,                    \
                                status);                                  \
    else                                                                  \
        UVM_DBG_PRINT("PASS: "#TestFunction);                             \
} while (0)

NV_STATUS gpuOpsSampleTest(NvProcessorUuid * pUuidStruct);

NV_STATUS regionTrackerSanityTest(void);

NV_STATUS uvmtest_channel_basic_migration(UvmChannelManager *pMgr);

NV_STATUS uvmtest_channel_pushbuffer_sanity(UvmChannelManager *pMgr);

NV_STATUS uvmtest_channel_pushbuffer_inline(UvmChannelManager *pMgr);

NV_STATUS uvmtest_channel_directed(UvmChannelManager *pMgr);

NV_STATUS uvmtest_channel_physical_migration(UvmChannelManager *channelManager);

NV_STATUS uvmtest_channel_pagesize_directed(UvmChannelManager *channelManager,
                                            enum UvmtestMemblockFlag *pagesize);
NV_STATUS uvmtest_channel_p2p_migration(UvmChannelManager *channelManager,
                                        UvmChannelManager *peerChannelManager,
                                        NvU32 peerId);

#endif // _UVM_GPU_OPS_TESTS_H_
