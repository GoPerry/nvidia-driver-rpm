/*******************************************************************************
    Copyright (c) 2016 NVIDIA Corporation

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
#ifndef __NV_MM_H__
#define __NV_MM_H__

/*  get_user_pages_remote() was added by:
 *    2016 Feb 12: 1e9877902dc7e11d2be038371c6fbf2dfcd469d7
 *
 *  The very next commit (cde70140fed8429acf7a14e2e2cbd3e329036653)
 *  deprecated the 8-argument version of get_user_pages for the
 *  non-remote case (calling get_user_pages with current and current->mm).
 *
 *  The guidelines are: call NV_GET_USER_PAGES_REMOTE if you need the 8-argument
 *  version that uses something other than current and current->mm. Use
 *  NV_GET_USER_PAGES if you are refering to current and current->mm.
 *
*  Note that get_user_pages_remote() requires the caller to hold a reference on
*  the task_struct (if non-NULL) and the mm_struct. This will always be true
*  when using current and current->mm. If the kernel passes the driver a vma
*  via driver callback, the kernel holds a reference on vma->vm_mm over that
*  callback.
 */

#if defined(NV_GET_USER_PAGES_REMOTE_PRESENT)
    #define NV_GET_USER_PAGES           get_user_pages
    #define NV_GET_USER_PAGES_REMOTE    get_user_pages_remote
#else
    #define NV_GET_USER_PAGES(start, nr_pages, write, force, pages, vmas) \
        get_user_pages(current, current->mm, start, nr_pages, write, force, pages, vmas)

    #define NV_GET_USER_PAGES_REMOTE    get_user_pages
#endif


#endif // __NV_MM_H__
