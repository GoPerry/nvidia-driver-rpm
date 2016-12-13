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

#ifndef _NVLINK_PROTO_H_
#define _NVLINK_PROTO_H_

/*
 * Functions defined in nvlink_linux.c
 */

int           nvlink_core_init           (void);
void          nvlink_core_exit           (void);

/*
 * Functions defined in ebridge_linux.c
 */
int           ebridge_init          (void);
void          ebridge_exit          (void);

/*
 * Functions defined in ibmnpu_linux.c
 */
int           ibmnpu_init          (void);
void          ibmnpu_exit          (void);

#endif /* _NVLINK_PROTO_H_ */