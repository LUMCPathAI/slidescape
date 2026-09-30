/*
  BSD 2-Clause License

  Copyright (c) 2019-2026, Pieter Valkema

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
     list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <semaphore.h>
#endif

typedef void (work_queue_callback_t)(int logical_thread_index, void* userdata);
typedef u32 completion_event_kind_t;
typedef struct thread_pool_t thread_pool_t;

typedef struct task_group_t {
	i32 volatile pending_count;
	struct task_group_t* parent;
} task_group_t;

typedef struct work_queue_entry_t {
	bool32 is_valid;
	u32 task_identifier;
	work_queue_callback_t* callback;
	task_group_t* task_group;
	void* heap_userdata;
	size_t userdata_size;
	// do_work passes this local copy directly to callbacks. Preserve malloc-like
	// alignment for payload structs, including 16-byte SIMD members.
#if defined(_MSC_VER)
	__declspec(align(16)) u8 userdata[128];
#else
	u8 userdata[128] __attribute__((aligned(16)));
#endif
} work_queue_entry_t;

typedef struct completion_event_t {
	bool32 is_valid;
	completion_event_kind_t kind;
	u8 userdata[128];
} completion_event_t;

typedef struct completion_queue_t {
	i32 volatile next_entry_to_submit;
	i32 volatile next_entry_to_read;
	i32 entry_count;
	completion_event_t* entries;
} completion_queue_t;

typedef struct work_queue_t {
#if WINDOWS
	HANDLE semaphore;
#else
	sem_t* semaphore;
#endif
	// Stable, cache-aligned allocation; constructors return this handle by value.
	struct work_queue_state_t* state;
	i32 entry_count; // usable capacity, rounded up to a power of two (minimum 2)
	thread_pool_t* owner_pool;
	i32 logical_thread_index; // index of the thread that created/owns this work queue
	bool owns_semaphore;
} work_queue_t;


typedef void (thread_pool_thread_init_callback_t)(int logical_thread_index, void* userdata);
struct thread_pool_t {
	work_queue_t* queue;
	work_queue_t* high_priority_queue;
    i32 volatile initialized;
	i32 volatile active; // setting to 0 flags the thread pool for destruction
	// Both priority queues share one semaphore and one waiter-registration count.
	i32 volatile sleeping_worker_count;
	i32 total_worker_thread_count;
	i32 volatile active_worker_thread_count; // use the getter/setter once initialized
	bool need_init_async_io_events;
	thread_pool_thread_init_callback_t* thread_init_callback;
#if WINDOWS
	HANDLE* thread_handles;
#else
	pthread_t* thread_handles;
#endif
};

// Bounded multi-producer/multi-consumer FIFO. Capacity is rounded up to a power
// of two. A stalled producer can delay consumption of later entries: mutex-free,
// but not a formal lock-free progress guarantee. No live handle may be copied.
work_queue_t work_queue_create(const char* semaphore_name, i32 entry_count);
work_queue_t work_queue_create_with_existing_semaphore(void* semaphore_handle, i32 entry_count);
// Caller must stop all producers/consumers first. Drain grouped tasks before
// destroying their queue; destruction frees queued userdata, not task-group refs.
void work_queue_destroy(work_queue_t* queue);
// Approximate concurrent snapshot, including reserved but unpublished entries.
i32 work_queue_get_entry_count(work_queue_t* queue);
// Submission copies userdata; false means full (possibly a slot still being
// copied by a consumer) or allocation failure. Caller retains its original data.
// A successful submission adds a group reference before publishing the callback.
bool work_queue_submit_task(work_queue_t* queue, work_queue_callback_t callback, void* userdata, size_t userdata_size);
bool work_queue_submit_task_to_group(work_queue_t* queue, task_group_t* task_group, work_queue_callback_t callback, void* userdata, size_t userdata_size);
bool work_queue_submit_notification(work_queue_t* queue, u32 task_identifier, void* userdata, size_t userdata_size);
bool work_queue_submit(work_queue_t* queue, work_queue_callback_t callback, u32 task_identifier, void* userdata, size_t userdata_size);
bool work_queue_submit_to_group(work_queue_t* queue, task_group_t* task_group, work_queue_callback_t callback, u32 task_identifier, void* userdata, size_t userdata_size);
// Manual consumers own the returned copy, must free heap_userdata, end its
// task_group and mark it completed exactly once. Invalid means no ready FIFO head.
// Callbacks instead use do_work(), which handles all of this automatically.
work_queue_entry_t work_queue_get_next_entry(work_queue_t* queue);
void work_queue_mark_entry_completed(work_queue_t* queue);
bool work_queue_do_work(work_queue_t* queue);
// Completion acquires callback writes. A false snapshot cannot rule out future
// submissions; stop external producers before waiting for completion/destruction.
bool work_queue_is_work_in_progress(work_queue_t* queue);
bool work_queue_is_work_waiting_to_start(work_queue_t* queue);
completion_queue_t completion_queue_create(i32 entry_count);
void completion_queue_destroy(completion_queue_t* queue);
bool completion_queue_post(completion_queue_t* queue, completion_event_kind_t kind, void* userdata, size_t userdata_size);
bool completion_queue_poll(completion_queue_t* queue, completion_event_t* out_event);
bool completion_queue_has_events(completion_queue_t* queue);
// A waiter may release the group as soon as pending_count reaches zero. Finish
// external submissions before waiting, or keep a begin/end reference while adding
// tasks. Nested callbacks can safely add children while their own reference lives.
void task_group_begin(task_group_t* group);
void task_group_end(task_group_t* group);
bool task_group_is_complete(task_group_t* group);
task_group_t task_group_create_child(task_group_t* parent);
void dummy_work_queue_callback(int logical_thread_index, void* userdata);
void test_multithreading_work_queue();
void init_thread_pool(thread_pool_t* pool, i32 work_queue_max_entry_count, bool need_high_priority_queue, bool need_init_async_io_events, thread_pool_thread_init_callback_t thread_init_callback);
work_queue_t* thread_pool_get_queue(thread_pool_t* pool);
work_queue_t* thread_pool_get_high_priority_queue(thread_pool_t* pool);
i32 thread_pool_get_task_count(thread_pool_t* pool);
i32 thread_pool_get_task_capacity(thread_pool_t* pool);
i32 thread_pool_get_worker_thread_count(thread_pool_t* pool);
i32 thread_pool_get_active_worker_thread_count(thread_pool_t* pool);
// Takes effect between callbacks; previously disabled workers check every 100 ms.
void thread_pool_set_active_worker_thread_count(thread_pool_t* pool, i32 count);
// Scheduling estimate, not an exact worker census: includes caller-executed and
// nested callbacks as busy work, clamped to the enabled worker count.
i32 thread_pool_get_idle_worker_thread_count(thread_pool_t* pool);
bool thread_pool_submit_task(thread_pool_t* pool, work_queue_callback_t callback, void* userdata, size_t userdata_size);
bool thread_pool_submit_task_to_group(thread_pool_t* pool, task_group_t* task_group, work_queue_callback_t callback, void* userdata, size_t userdata_size);
bool thread_pool_submit_high_priority_task(thread_pool_t* pool, work_queue_callback_t callback, void* userdata, size_t userdata_size);
bool thread_pool_submit_high_priority_task_to_group(thread_pool_t* pool, task_group_t* task_group, work_queue_callback_t callback, void* userdata, size_t userdata_size);
bool thread_pool_do_work(thread_pool_t* pool);
void thread_pool_wait_for_group(thread_pool_t* pool, task_group_t* group);
bool thread_pool_is_work_in_progress(thread_pool_t* pool);
bool thread_pool_is_work_waiting_to_start(thread_pool_t* pool);
void thread_pool_wait_for_completion(thread_pool_t* pool);
void thread_pool_destroy(thread_pool_t* pool);
#ifdef LIBISYNTAX_THREAD_POOL_SHARED_WITH_SLIDESCAPE
void libisyntax_init_thread_pool_for_slidescape(void);
#endif


// globals
#if defined(WORK_QUEUE_IMPL)
#define INIT(...) __VA_ARGS__
#define extern
#else
#define INIT(...)
#undef extern
#endif

extern thread_pool_t global_thread_pool;
extern completion_queue_t global_completion_queue;


#undef INIT
#undef extern



#ifdef __cplusplus
}
#endif
