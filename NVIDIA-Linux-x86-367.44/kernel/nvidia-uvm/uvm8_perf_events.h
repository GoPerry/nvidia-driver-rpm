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

#ifndef __UVM8_PERF_EVENTS_H__
#define __UVM8_PERF_EVENTS_H__

#include "uvm_linux.h"
#include "uvm8_forward_decl.h"
#include "uvm8_processors.h"
#include "uvm8_hal_types.h"
#include "uvm8_lock.h"
#include "uvm8_va_block_types.h"

// uvm_perf_events is an event notification dispatcher that broadcasts events to clients. Clients register functions
// to be called under specific events. The callback lists are stored per va_space and, therefore, different callbacks
// can be registered per client. This will be useful to use different performance heuristic implementations depending
// on the GPU resources used by each process. For example, on a system with Pascal + Kepler GPUs, VA spaces which
// have Kepler GPU VA spaces will be restrited to the UVM-Lite feature set, while a VA space which only uses the Pascal
// GPU will not be downgraded. Registering/unregistering callbacks requires holding the VA space events lock in write
// mode. The exact locking guarantees under which callbacks are executed depend on the specific event, but the VA space
// events lock is held in read mode for all of them. The additional locking guarantees are defined in each event
// definition.

// Performance-related events that can be notified
typedef enum
{
    // Locking: uvm_va_space: at least in read mode, uvm_va_block: exclusive / nobody is referencing the block anymore
    UVM_PERF_EVENT_BLOCK_DESTROY = 0,

    // Locking: uvm_va_space: write
    UVM_PERF_EVENT_BLOCK_SHRINK,

    // Locking: uvm_va_space: write
    UVM_PERF_EVENT_RANGE_DESTROY,

    // Locking: uvm_va_space: write
    UVM_PERF_EVENT_RANGE_SHRINK,

    // Locking: uvm_va_space: write
    UVM_PERF_EVENT_SPACE_DESTROY,

    // Locking: uvm_va_space: write
    UVM_PERF_EVENT_MODULE_UNLOAD,

    // Locking: uvm_va_space: at least in read mode, uvm_va_block: exclusive (if uvm_va_block is not NULL)
    UVM_PERF_EVENT_FAULT,

    // Locking: uvm_va_block: exclusive. Notably the uvm_va_space lock may not be held on eviction.
    UVM_PERF_EVENT_MIGRATION,

    // Locking: uvm_va_space: at least in read mode, uvm_va_block: exclusive
    UVM_PERF_EVENT_REVOCATION,

    // Locking: uvm_va_space: at least in read mode, uvm_va_block: exclusive
    UVM_PERF_EVENT_FIRST_TOUCH,

    // Locking: uvm_va_space: at least in read mode, uvm_va_block: exclusive
    UVM_PERF_EVENT_BLOCK_MIGRATION_BEGIN,

    UVM_PERF_EVENT_COUNT,
} uvm_perf_event_t;

// Format of the data passed to callbacks. Scope must be filled with the appropriate values by the code which notifies
// the event
typedef union
{
    struct
    {
        uvm_va_block_t *block;
    } block_destroy;

    struct
    {
        uvm_va_block_t *block;
    } block_shrink;

    struct
    {
        uvm_va_range_t *range;
    } range_destroy;

    struct
    {
        uvm_va_range_t *range;
    } range_shrink;

    struct
    {
        uvm_va_space_t *space;
    } space_destroy;

    struct
    {
        uvm_perf_module_t *module;

        // Only one of these three can be set. The rest must be NULL
        uvm_va_block_t *block;
        uvm_va_range_t *range;
        uvm_va_space_t *space;
    } module_unload;

    struct
    {
        // VA block for the page where the fault was triggered if it exists, NULL otherwise (this can happen if the
        // fault is fatal or the VA block could not be created)
        uvm_va_block_t *block;

        // This field contains the VA space where this fault was reported. Is block is not NULL, this field must match
        // block->va_range->va_space
        uvm_va_space_t *space;

        uvm_processor_id_t proc_id;

        // Fault descriptor
        union
        {
            struct
            {
                uvm_fault_buffer_entry_t *buffer_entry;

                NvU32 batch_id;
            } gpu;

            struct
            {
                NvU64 fault_va;

                bool is_write;
            } cpu;
        };
    } fault;

    struct
    {
        uvm_push_t *push;
        uvm_va_block_t *block;

        // ID of the source processor of the migration
        uvm_processor_id_t src;

        // ID of the destination processor of the migration
        uvm_processor_id_t dst;

        // Start address of the memory range being migrated
        NvU64 address;

        // Number of bytes being migrated
        NvU64 bytes;

        // Whether the page has been copied or moved
        uvm_va_block_transfer_mode_t transfer_mode;

        // Event that caused the migration
        UvmEventMigrationCause cause;
    } migration;

    struct
    {
        uvm_va_block_t *block;

        // ID of the processor whose access permissions have been revoked
        uvm_processor_id_t proc_id;

        // Start address of the memory range being revoked
        NvU64 address;

        // Number of bytes of the memory range being revoked
        NvU64 bytes;

        // Old access permission
        uvm_prot_t old_prot;

        // New access permission
        uvm_prot_t new_prot;
    } revocation;

    struct
    {
        uvm_va_block_t *block;

        // Memory where the memory was allocated on first touch
        uvm_processor_id_t dst;

        // Start address of the memory range that was allocated on first touch
        NvU64 address;

        // Number of bytes of the memory range that was allocated on first touch
        NvU64 bytes;
    } first_touch;
} uvm_perf_event_data_t;

// Type of the function that can be registered as a callback
//
// event_id: is the event being notified. Passing it to the callback enables using the same function to handle
//           different events.
// event_data: extra event data that is passed to the callback function. The format of data passed for each event type
//             is declared in the uvm_perf_event_data_t union
typedef void (*uvm_perf_event_callback_t)(uvm_perf_event_t event_id, uvm_perf_event_data_t *event_data);

typedef struct
{
    // Lock protecting the events
    //
    // Held for write during registration/unregistration of callbacks and for
    // read during notification of events.
    //
    // Also used by tools to protect their state and registration of perf event callbacks.
    uvm_rw_semaphore_t lock;

    // Array of callbacks for event notification
    struct list_head event_callbacks[UVM_PERF_EVENT_COUNT];

    uvm_va_space_t *va_space;
} uvm_perf_va_space_events_t;

// Initialize event notifiction for a va_space. This must be called from va_space construction. No locking required
NV_STATUS uvm_perf_init_va_space_events(uvm_va_space_t *va_space, uvm_perf_va_space_events_t *va_space_events);

// Finalize event notifiction for a va_space. Caller must hold va_space lock in write mode
void uvm_perf_destroy_va_space_events(uvm_perf_va_space_events_t *va_space_events);

// Register a callback to be executed under the given event. The given callback cannot have been already registered for
// the same event, although the same callback can be registered for different events.
NV_STATUS uvm_perf_register_event_callback(uvm_perf_va_space_events_t *va_space_events,
                                           uvm_perf_event_t event_id, uvm_perf_event_callback_t cb);

// Same as uvm_perf_register_event_callback(), but the caller must hold
// va_space_events lock in write mode.
NV_STATUS uvm_perf_register_event_callback_locked(uvm_perf_va_space_events_t *va_space_events,
                                                  uvm_perf_event_t event_id, uvm_perf_event_callback_t cb);

// Removes a callback for the given event. It's safe to call with a callback that hasn't been registered.
void uvm_perf_unregister_event_callback(uvm_perf_va_space_events_t *va_space_events, uvm_perf_event_t event_id,
                                        uvm_perf_event_callback_t cb);

// Same as uvm_perf_unregister_event_callback(), but the caller must hold
// va_space_events lock in write mode.
void uvm_perf_unregister_event_callback_locked(uvm_perf_va_space_events_t *va_space_events, uvm_perf_event_t event_id,
                                               uvm_perf_event_callback_t cb);

// Invoke the callbacks registered for the given event. Callbacks cannot fail.
// Acquires the va_space_events lock internally
void uvm_perf_event_notify(uvm_perf_va_space_events_t *va_space_events, uvm_perf_event_t event_id,
                           uvm_perf_event_data_t *event_data);

// Initialization/cleanup functions
NV_STATUS uvm_perf_events_init(void);
void uvm_perf_events_exit(void);

#endif
