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

#define WORK_QUEUE_IMPL
#include "work_queue.h"
#include "platform.h"
#include "intrinsics.h"

#if defined(_WIN32)
#include <windows.h>
#include "win32_utils.h" // for win32_diagnostic()
#else
#include <semaphore.h>
#include <pthread.h>
#endif

work_queue_t work_queue_create(const char* semaphore_name, i32 entry_count) {
	work_queue_t queue = {0};

	queue.logical_thread_index = threadlocal_logical_thread_index;

	i32 semaphore_initial_count = 0;
#if WINDOWS
	(void)semaphore_name;
	LONG maximum_count = 1e6; // realistically, we'd only get up to the number of worker threads, though
	/* Each pool needs an independent wake-up count. A fixed named semaphore
	 * lets unrelated pools consume each other's task and shutdown signals. */
	queue.semaphore = CreateSemaphoreExA(
		0, semaphore_initial_count, maximum_count, NULL, 0, SEMAPHORE_ALL_ACCESS);
#elif APPLE
	// Prevent name collisions by appending a unique number
	char semaphore_name_unique[64];
	static volatile i32 sem_id = 0;
	snprintf(semaphore_name_unique, sizeof(semaphore_name_unique), "%s%lld", semaphore_name, atomic_increment(&sem_id));
	queue.semaphore = sem_open(semaphore_name_unique, O_CREAT, 0644, semaphore_initial_count);
	sem_unlink(semaphore_name_unique);
#else
	queue.semaphore = calloc(1, sizeof(sem_t));
	i32 rc = sem_init(queue.semaphore, 0, 0);
	if (rc != 0) {
		perror("sem_init");
		fatal_error("failed to initialize semaphore");
	}
#endif
	queue.owns_semaphore = true;
	queue.entry_count = entry_count + 1; // add safety margin to detect when queue is about to overflow
	queue.entries = calloc(1, (entry_count + 1) * sizeof(work_queue_entry_t));
	queue.entries_lock = malloc(sizeof(*queue.entries_lock));
	if (!queue.entries || !queue.entries_lock) fatal_error("work queue allocation failed");
	platform_mutex_init(queue.entries_lock);
	return queue;
}

work_queue_t work_queue_create_with_existing_semaphore(void* semaphore_handle, i32 entry_count) {
	work_queue_t queue = {0};
#if WINDOWS
	queue.semaphore = (HANDLE)semaphore_handle;
#else
	queue.semaphore = (sem_t*)semaphore_handle;
#endif
	queue.owns_semaphore = false;
	queue.entry_count = entry_count + 1; // add safety margin to detect when queue is about to overflow
	queue.entries = calloc(1, (entry_count + 1) * sizeof(work_queue_entry_t));
	queue.entries_lock = malloc(sizeof(*queue.entries_lock));
	if (!queue.entries || !queue.entries_lock) fatal_error("work queue allocation failed");
	platform_mutex_init(queue.entries_lock);
	return queue;
}

void work_queue_destroy(work_queue_t* queue) {
	if (!queue) {
		return;
	}
	if (queue->entries) {
		for (i32 i = 0; i < queue->entry_count; ++i) {
			if (queue->entries[i].heap_userdata) {
				free(queue->entries[i].heap_userdata);
			}
		}
		free(queue->entries);
		queue->entries = NULL;
	}
	if (queue->entries_lock) {
		platform_mutex_destroy(queue->entries_lock);
		free(queue->entries_lock);
	}
	if (!queue->owns_semaphore || !queue->semaphore) {
		memset(queue, 0, sizeof(work_queue_t));
		return;
	}
#if WINDOWS
	CloseHandle(queue->semaphore);
#elif APPLE
	sem_close(queue->semaphore);
#else
	sem_destroy(queue->semaphore);
	free(queue->semaphore);
#endif
	queue->semaphore = NULL;
	memset(queue, 0, sizeof(work_queue_t));
}

i32 work_queue_get_entry_count(work_queue_t* queue) {
	platform_mutex_lock(queue->entries_lock);
	i32 count = queue->next_entry_to_submit - queue->next_entry_to_execute;
	while (count < 0) {
		count += queue->entry_count;
	}
	platform_mutex_unlock(queue->entries_lock);
	return count;
}

void task_group_begin(task_group_t* group) {
	if (group) {
		atomic_add(&group->pending_count, 1);
		if (group->parent) {
			task_group_begin(group->parent);
		}
	}
}

void task_group_end(task_group_t* group) {
	if (group) {
		// A waiter may destroy a stack-allocated group immediately at zero.
		task_group_t* parent = group->parent;
		atomic_add(&group->pending_count, -1);
		if (parent) task_group_end(parent);
	}
}

bool task_group_is_complete(task_group_t* group) {
	return !group || atomic_load_acquire(&group->pending_count) == 0;
}

task_group_t task_group_create_child(task_group_t* parent) {
	task_group_t result = {.parent = parent};
	return result;
}

// TODO: add optional refcount increment
bool work_queue_submit_to_group(work_queue_t* queue, task_group_t* task_group, work_queue_callback_t callback, u32 task_identifier, void* userdata, size_t userdata_size) {
	if (!queue) {
		fatal_error("work_queue_add_entry(): queue is NULL");
	}
	void* heap_userdata = NULL;
	if (userdata_size > sizeof(((work_queue_entry_t*)0)->userdata)) {
		heap_userdata = malloc(userdata_size);
		if (!heap_userdata) {
			console_print_error("work_queue_add_entry(): failed to allocate %zu bytes for userdata\n", userdata_size);
			return false;
		}
		ASSERT(userdata);
		memcpy(heap_userdata, userdata, userdata_size);
	}
	// Publish the payload and counters together. Consumers must finish copying a
	// slot before producers can reuse it; CAS on the ring indices alone cannot
	// guarantee that, and wrapped indices also admit ABA between consumers.
	platform_mutex_lock(queue->entries_lock);
	i32 entry_to_submit = queue->next_entry_to_submit;
	i32 new_next_entry_to_submit = (entry_to_submit + 1) % queue->entry_count;
	if (new_next_entry_to_submit == queue->next_entry_to_execute) {
		platform_mutex_unlock(queue->entries_lock);
		free(heap_userdata);
		return false; // Caller can retry or cancel; ownership was not transferred.
	}
	task_group_begin(task_group);
	work_queue_entry_t* entry = queue->entries + entry_to_submit;
	*entry = (work_queue_entry_t){ .is_valid = true, .callback = callback,
	        .task_identifier = task_identifier, .task_group = task_group,
	        .userdata_size = userdata_size, .heap_userdata = heap_userdata };
	if (userdata_size > 0 && !heap_userdata) {
		ASSERT(userdata);
		memcpy(entry->userdata, userdata, userdata_size);
	}
	++queue->completion_goal;
	++queue->start_goal;
	queue->next_entry_to_submit = new_next_entry_to_submit;
	platform_mutex_unlock(queue->entries_lock);
	platform_semaphore_post(queue->semaphore);
	return true;
}

bool work_queue_submit(work_queue_t* queue, work_queue_callback_t callback, u32 task_identifier, void* userdata, size_t userdata_size) {
	return work_queue_submit_to_group(queue, NULL, callback, task_identifier, userdata, userdata_size);
}

bool work_queue_submit_task(work_queue_t* queue, work_queue_callback_t callback, void* userdata, size_t userdata_size) {
	ASSERT(callback);
	return work_queue_submit(queue, callback, 0, userdata, userdata_size);
}

bool work_queue_submit_task_to_group(work_queue_t* queue, task_group_t* task_group, work_queue_callback_t callback, void* userdata, size_t userdata_size) {
	ASSERT(callback);
	return work_queue_submit_to_group(queue, task_group, callback, 0, userdata, userdata_size);
}

bool work_queue_submit_notification(work_queue_t* queue, u32 task_identifier, void* userdata, size_t userdata_size) {
	return work_queue_submit(queue, NULL, task_identifier, userdata, userdata_size);
}

work_queue_entry_t work_queue_get_next_entry(work_queue_t* queue) {
	work_queue_entry_t result = {0};
	platform_mutex_lock(queue->entries_lock);
	i32 entry_to_execute = queue->next_entry_to_execute;
	if (entry_to_execute != queue->next_entry_to_submit) {
		result = queue->entries[entry_to_execute];
		queue->entries[entry_to_execute] = (work_queue_entry_t){0};
		queue->next_entry_to_execute = (entry_to_execute + 1) % queue->entry_count;
	}
	platform_mutex_unlock(queue->entries_lock);
	return result;
}

void work_queue_mark_entry_completed(work_queue_t* queue) {
	platform_mutex_lock(queue->entries_lock);
	++queue->completion_count;
	platform_mutex_unlock(queue->entries_lock);
}


bool work_queue_do_work(work_queue_t* queue) {
    if (!queue) {
        return false;
    }
	work_queue_entry_t entry = work_queue_get_next_entry(queue);
	if (entry.is_valid) {
		if (queue->owner_pool) {
			atomic_decrement(&queue->owner_pool->worker_thread_idle_count);
		}
		platform_mutex_lock(queue->entries_lock);
		++queue->start_count;
		platform_mutex_unlock(queue->entries_lock);
		ASSERT(entry.callback);
		if (entry.callback) {
			// Copy the user data (arguments for the call) onto the stack
			void* userdata = entry.heap_userdata;
			if (userdata == NULL) {
				userdata = alloca(sizeof(entry.userdata));
				memcpy(userdata, entry.userdata, sizeof(entry.userdata));
			}

			// Ensure all the memory allocated on the thread's temp_arena will be released when the task completes
			temp_memory_t temp = begin_temp_memory_on_local_thread();

			// Execute the task
			entry.callback(threadlocal_logical_thread_index, userdata);

			release_temp_memory(&temp);
			if (entry.heap_userdata) {
				free(entry.heap_userdata);
			}
		}
		task_group_end(entry.task_group);
		if (queue->owner_pool) {
			atomic_increment(&queue->owner_pool->worker_thread_idle_count);
		}
		work_queue_mark_entry_completed(queue);
	}
	return entry.is_valid;
}


bool work_queue_is_work_in_progress(work_queue_t* queue) {
	if (!queue) {
		return false;
	}
	platform_mutex_lock(queue->entries_lock);
	bool result = (queue->completion_goal != queue->completion_count);
	platform_mutex_unlock(queue->entries_lock);
	return result;
}

bool work_queue_is_work_waiting_to_start(work_queue_t* queue) {
	if (!queue) {
		return false;
	}
	platform_mutex_lock(queue->entries_lock);
	bool result = (queue->next_entry_to_submit != queue->next_entry_to_execute);
	platform_mutex_unlock(queue->entries_lock);
	return result;
}

completion_queue_t completion_queue_create(i32 entry_count) {
	completion_queue_t queue = {0};
	queue.entry_count = entry_count + 1;
	queue.entries = calloc(1, queue.entry_count * sizeof(completion_event_t));
	return queue;
}

void completion_queue_destroy(completion_queue_t* queue) {
	if (!queue) {
		return;
	}
	if (queue->entries) {
		free(queue->entries);
	}
	memset(queue, 0, sizeof(*queue));
}

bool completion_queue_post(completion_queue_t* queue, completion_event_kind_t kind, void* userdata, size_t userdata_size) {
	if (!queue) {
		fatal_error("completion_queue_post(): queue is NULL");
	}
	if (userdata_size > sizeof(((completion_event_t*)0)->userdata)) {
		fatal_error("completion_queue_post(): userdata_size overflows available space");
	}
	for (i32 tries = 0; tries < 1000; ++tries) {
		i32 entry_to_submit = queue->next_entry_to_submit;
		i32 new_next_entry_to_submit = (queue->next_entry_to_submit + 1) % queue->entry_count;
		if (new_next_entry_to_submit == queue->next_entry_to_read) {
			console_print_error("Warning: completion queue is overflowing - event is cancelled\n");
			return false;
		}

		bool succeeded = atomic_compare_exchange(&queue->next_entry_to_submit,
		                                         new_next_entry_to_submit, entry_to_submit);
		if (succeeded) {
			completion_event_t* entry = queue->entries + entry_to_submit;
			*entry = (completion_event_t){ .kind = kind };
			if (userdata_size > 0) {
				ASSERT(userdata);
				memcpy(entry->userdata, userdata, userdata_size);
			}
			write_barrier;
			entry->is_valid = true;
			return true;
		}
	}
	return false;
}

bool completion_queue_poll(completion_queue_t* queue, completion_event_t* out_event) {
	if (!queue) {
		return false;
	}
	i32 entry_to_read = queue->next_entry_to_read;
	i32 new_next_entry_to_read = (entry_to_read + 1) % queue->entry_count;
	if ((entry_to_read != queue->next_entry_to_submit) && (queue->entries[entry_to_read].is_valid)) {
		completion_event_t* entry = queue->entries + entry_to_read;
		if (out_event) {
			*out_event = *entry;
		}
		entry->is_valid = false;
		write_barrier;
		queue->next_entry_to_read = new_next_entry_to_read;
		return true;
	}
	return false;
}

bool completion_queue_has_events(completion_queue_t* queue) {
	if (!queue) {
		return false;
	}
	i32 entry_to_read = queue->next_entry_to_read;
	return (entry_to_read != queue->next_entry_to_submit) && queue->entries[entry_to_read].is_valid;
}

void dummy_work_queue_callback(int logical_thread_index, void* userdata) {}

//#define TEST_THREAD_QUEUE
#ifdef TEST_THREAD_QUEUE
void echo_task_completed(int logical_thread_index, void* userdata) {
	console_print("thread %d completed: %s\n", logical_thread_index, (char*) userdata);
}

void echo_task(int logical_thread_index, void* userdata) {
	console_print("thread %d: %s\n", logical_thread_index, (char*) userdata);

	completion_queue_post(&global_completion_queue, 1, userdata, strlen(userdata)+1);
}
#endif

void test_multithreading_work_queue() {
#ifdef TEST_THREAD_QUEUE
	thread_pool_submit_task(&global_thread_pool, echo_task, (void*)"NULL entry", 11);
	thread_pool_submit_task(&global_thread_pool, echo_task, (void*)"string 0", 9);
	thread_pool_submit_task(&global_thread_pool, echo_task, (void*)"string 1", 9);
	thread_pool_submit_task(&global_thread_pool, echo_task, (void*)"string 2", 9);
	thread_pool_submit_task(&global_thread_pool, echo_task, (void*)"string 3", 9);
	thread_pool_submit_task(&global_thread_pool, echo_task, (void*)"string 4", 9);
	thread_pool_submit_task(&global_thread_pool, echo_task, (void*)"string 5", 9);
	thread_pool_submit_task(&global_thread_pool, echo_task, (void*)"string 6", 9);
	thread_pool_submit_task(&global_thread_pool, echo_task, (void*)"string 7", 9);
	thread_pool_submit_task(&global_thread_pool, echo_task, (void*)"string 8", 9);
	thread_pool_submit_task(&global_thread_pool, echo_task, (void*)"string 9", 9);
	thread_pool_submit_task(&global_thread_pool, echo_task, (void*)"string 10", 10);
	thread_pool_submit_task(&global_thread_pool, echo_task, (void*)"string 11", 10);

	while (thread_pool_is_work_in_progress(&global_thread_pool) || completion_queue_has_events(&global_completion_queue)) {
		completion_event_t event = {0};
		completion_queue_poll(&global_completion_queue, &event);
	}
#endif
}

typedef struct work_pool_thread_create_info_t {
	i32 logical_thread_index;
	thread_pool_t* pool;
} work_pool_thread_create_info_t;

#if WINDOWS
static DWORD WINAPI worker_thread(void* parameter) {
#else
static void* worker_thread(void* parameter) {
#endif
	work_pool_thread_create_info_t* thread_info = (work_pool_thread_create_info_t*) parameter;
	i32 logical_thread_index = thread_info->logical_thread_index;
	threadlocal_logical_thread_index = logical_thread_index;
	thread_pool_t* pool = thread_info->pool;
	free(thread_info);

//	fprintf(stderr, "Hello from thread %d\n", threadlocal_logical_thread_index);

	init_thread_memory(&global_system_info);
	atomic_increment(&pool->worker_thread_idle_count);

	if (pool->thread_init_callback) {
		// NOTE: worker threads might want to do additional setup at this point, e.g. OpenGL initialization
		// TODO: what might we need to pass as userdata here? maybe not even needed?
		pool->thread_init_callback(logical_thread_index, NULL);
	}
#if WINDOWS
	// TODO: revise this
	if (pool->need_init_async_io_events) {
		thread_memory_t* thread_memory = threadlocal_thread_memory;
		for (i32 i = 0; i < MAX_ASYNC_IO_EVENTS; ++i) {
			thread_memory->async_io_events[i] = CreateEventA(NULL, TRUE, FALSE, NULL);
			if (!thread_memory->async_io_events[i]) {
				win32_diagnostic("CreateEvent");
			}
		}
	}
#endif

	for (;;) {
		if (!pool->active) {
			atomic_decrement(&pool->refcount);
			break;
		}

		if (logical_thread_index > pool->active_worker_thread_count) {
			// Worker is disabled, do nothing
			platform_sleep(100);
			continue;
		}

		if (!work_queue_do_work(pool->high_priority_queue)) {
			if (!work_queue_do_work(pool->queue)) {
				if (!(work_queue_is_work_waiting_to_start(pool->queue) || work_queue_is_work_waiting_to_start(pool->high_priority_queue))) {
					platform_semaphore_wait(pool->queue->semaphore);
				}
			}
		}

	}

	destroy_thread_memory();
	return 0;
}


static platform_mutex_t work_pool_global_mutex = PLATFORM_MUTEX_INITIALIZER;

void init_thread_pool(thread_pool_t* pool, i32 work_queue_max_entry_count, bool need_high_priority_queue, bool need_init_async_io_events, thread_pool_thread_init_callback_t thread_init_callback) {
	// Lock-unlock to ensure that all parallel calls to work_pool_init() wait for the actual initialization to complete.
	platform_mutex_lock(&work_pool_global_mutex);

	if (!pool->initialized) {
		// Actual initialization.
		//DBGCTR_COUNT(dbgctr_init_thread_pool_counter);
		if (threadlocal_logical_thread_index != 0) {
			console_print_error("init_thread_pool(): nested work pool creation not supported; tried to create from logical thread %d\n", threadlocal_logical_thread_index);
			fatal_error();
		}

		if (!threadlocal_thread_memory) {
			init_global_system_info(false);
			init_thread_memory(&global_system_info); // init thread memory for thread 0 (= main thread)
		}

		pool->queue = malloc(sizeof(work_queue_t));
		*pool->queue = work_queue_create("/worksem", work_queue_max_entry_count);
		pool->queue->owner_pool = pool;
		if (need_high_priority_queue) {
			pool->high_priority_queue = malloc(sizeof(work_queue_t));
			*pool->high_priority_queue = work_queue_create_with_existing_semaphore(pool->queue->semaphore, work_queue_max_entry_count);
			pool->high_priority_queue->owner_pool = pool;
		}
		pool->total_worker_thread_count = global_system_info.suggested_total_thread_count;
		pool->active_worker_thread_count = pool->total_worker_thread_count - 1;
		pool->need_init_async_io_events = need_init_async_io_events;
		pool->thread_init_callback = thread_init_callback;
		pool->active = 1;
		pool->thread_handles = calloc(global_system_info.suggested_total_thread_count, sizeof(*pool->thread_handles));

		// NOTE: the main thread is considered thread 0.
		for (i32 i = 1; i < global_system_info.suggested_total_thread_count; ++i) {
			// NOTE: we pass thread info to the worker thread via the heap; the worker thread will free it after use
			work_pool_thread_create_info_t* thread_info = malloc(sizeof(work_pool_thread_create_info_t));
			*thread_info = (work_pool_thread_create_info_t) {.pool = pool, .logical_thread_index = i};


#if WINDOWS
			DWORD thread_id;
			HANDLE thread_handle = CreateThread(NULL, 0, worker_thread, thread_info, 0, &thread_id);
			if (!thread_handle) {
				free(thread_info);
				fatal_error("CreateThread failed");
			}
			pool->thread_handles[i] = thread_handle;
#else
			pthread_t thread;
			if (pthread_create(&thread, NULL, &worker_thread, (void*)thread_info) != 0) {
				fprintf(stderr, "Error creating thread\n");
				fatal_error();
			}
			pool->thread_handles[i] = thread;

#endif
			++pool->refcount;
		}
		pool->initialized = true;
	}

	platform_mutex_unlock(&work_pool_global_mutex);

	test_multithreading_work_queue();

}

bool thread_pool_submit_task(thread_pool_t* pool, work_queue_callback_t callback, void* userdata, size_t userdata_size) {
	if (!pool || !pool->initialized) {
		return false;
	}
	return work_queue_submit_task(pool->queue, callback, userdata, userdata_size);
}

bool thread_pool_submit_task_to_group(thread_pool_t* pool, task_group_t* task_group, work_queue_callback_t callback, void* userdata, size_t userdata_size) {
	if (!pool || !pool->initialized) {
		return false;
	}
	return work_queue_submit_task_to_group(pool->queue, task_group, callback, userdata, userdata_size);
}

work_queue_t* thread_pool_get_queue(thread_pool_t* pool) {
	if (!pool || !pool->initialized) {
		return NULL;
	}
	return pool->queue;
}

work_queue_t* thread_pool_get_high_priority_queue(thread_pool_t* pool) {
	if (!pool || !pool->initialized) {
		return NULL;
	}
	return pool->high_priority_queue;
}

i32 thread_pool_get_task_count(thread_pool_t* pool) {
	if (!pool || !pool->initialized) {
		return 0;
	}
	return work_queue_get_entry_count(pool->queue);
}

i32 thread_pool_get_task_capacity(thread_pool_t* pool) {
	if (!pool || !pool->initialized || !pool->queue) {
		return 0;
	}
	return pool->queue->entry_count - 1;
}

i32 thread_pool_get_worker_thread_count(thread_pool_t* pool) {
	if (!pool || !pool->initialized) {
		return 0;
	}
	return pool->total_worker_thread_count - 1;
}

i32 thread_pool_get_active_worker_thread_count(thread_pool_t* pool) {
	if (!pool || !pool->initialized) {
		return 0;
	}
	return pool->active_worker_thread_count;
}

i32* thread_pool_get_active_worker_thread_count_ptr(thread_pool_t* pool) {
	if (!pool || !pool->initialized) {
		return NULL;
	}
	return &pool->active_worker_thread_count;
}

i32 thread_pool_get_idle_worker_thread_count(thread_pool_t* pool) {
	if (!pool || !pool->initialized) {
		return 0;
	}
	return pool->worker_thread_idle_count;
}

bool thread_pool_submit_high_priority_task(thread_pool_t* pool, work_queue_callback_t callback, void* userdata, size_t userdata_size) {
	if (!pool || !pool->initialized) {
		return false;
	}
	work_queue_t* queue = pool->high_priority_queue ? pool->high_priority_queue : pool->queue;
	return work_queue_submit_task(queue, callback, userdata, userdata_size);
}

bool thread_pool_submit_high_priority_task_to_group(thread_pool_t* pool, task_group_t* task_group, work_queue_callback_t callback, void* userdata, size_t userdata_size) {
	if (!pool || !pool->initialized) {
		return false;
	}
	work_queue_t* queue = pool->high_priority_queue ? pool->high_priority_queue : pool->queue;
	return work_queue_submit_task_to_group(queue, task_group, callback, userdata, userdata_size);
}

bool thread_pool_do_work(thread_pool_t* pool) {
	if (!pool || !pool->initialized) {
		return false;
	}
	if (work_queue_do_work(pool->high_priority_queue)) {
		return true;
	}
	return work_queue_do_work(pool->queue);
}

void thread_pool_wait_for_group(thread_pool_t* pool, task_group_t* group) {
	if (!pool || !pool->initialized) {
		return;
	}
	while (!task_group_is_complete(group)) {
		if (!thread_pool_do_work(pool)) {
			platform_sleep(1);
		}
	}
}

bool thread_pool_is_work_in_progress(thread_pool_t* pool) {
	if (!pool || !pool->initialized) {
		return false;
	}
	return work_queue_is_work_in_progress(pool->queue) || work_queue_is_work_in_progress(pool->high_priority_queue);
}

bool thread_pool_is_work_waiting_to_start(thread_pool_t* pool) {
	if (!pool || !pool->initialized) {
		return false;
	}
	return work_queue_is_work_waiting_to_start(pool->queue) || work_queue_is_work_waiting_to_start(pool->high_priority_queue);
}

void thread_pool_wait_for_completion(thread_pool_t* pool) {
	if (!pool || !pool->initialized) {
		return;
	}
	while (thread_pool_is_work_in_progress(pool)) {
		if (!thread_pool_do_work(pool)) {
			platform_sleep(1);
		}
	}
}

void thread_pool_destroy(thread_pool_t* pool) {
	if (!pool || !pool->initialized) {
		return;
	}
	if (threadlocal_logical_thread_index != 0) {
		fatal_error("thread_pool_destroy(): destroying a thread pool from a worker thread is not supported");
	}

	thread_pool_wait_for_completion(pool);
	pool->active = 0;
	write_barrier;

	for (i32 i = 1; i < pool->total_worker_thread_count; ++i) {
		platform_semaphore_post(pool->queue->semaphore);
	}

	if (pool->thread_handles) {
		for (i32 i = 1; i < pool->total_worker_thread_count; ++i) {
#if WINDOWS
			if (pool->thread_handles[i]) {
				WaitForSingleObject(pool->thread_handles[i], INFINITE);
				CloseHandle(pool->thread_handles[i]);
			}
#else
			pthread_join(pool->thread_handles[i], NULL);
#endif
		}
		free(pool->thread_handles);
		pool->thread_handles = NULL;
	}

	work_queue_destroy(pool->high_priority_queue);
	free(pool->high_priority_queue);
	work_queue_destroy(pool->queue);
	free(pool->queue);
	memset(pool, 0, sizeof(*pool));
}

#ifdef LIBISYNTAX_THREAD_POOL_SHARED_WITH_SLIDESCAPE
void libisyntax_init_thread_pool_for_slidescape(void) {
	// TODO: make this safer and refactor

	init_global_system_info(false);

	if (global_completion_queue.entries == NULL) {
		global_completion_queue = completion_queue_create(1024); // Message queue for completed tasks
	}

#if WINDOWS
	thread_pool_thread_init_callback_t* thread_init_callback = NULL; //&win32_thread_init_callback; //TODO?
#else
	thread_pool_thread_init_callback_t* thread_init_callback = NULL;
#endif

    init_thread_pool(&global_thread_pool, 1024, true, true, thread_init_callback);
}
#endif
