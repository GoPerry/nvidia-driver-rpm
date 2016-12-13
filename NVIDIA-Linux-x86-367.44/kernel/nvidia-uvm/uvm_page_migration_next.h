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

#ifndef UVM_KERNEL_UVM_PAGE_MIGRATION_NEXT_H_
#define UVM_KERNEL_UVM_PAGE_MIGRATION_NEXT_H_

#include "uvmtypes.h"

/*******************************************************************************
    NvUvmHalInit_Next

    Setup all the function pointers for the copy API for the "next" gpu.

    Argument:
        ceClass: (INPUT)
            Class of the copy engine.

        fifoClass: (INPUT)
            Class of the host engine.

        copyOps: (INPUT/OUPUT)
            CopyOps object with the function pointers.

*/
NV_STATUS NvUvmHalInit_Next(unsigned ceClass, unsigned fifoClass,
                        UvmCopyOps *copyOps);

#endif /* UVM_KERNEL_UVM_PAGE_MIGRATION_NEXT_H_ */
