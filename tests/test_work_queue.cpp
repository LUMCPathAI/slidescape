#include "doctest.h"

#include "platform.h"
#include "work_queue.h"
#include "intrinsics.h"

typedef struct test_counter_task_t {
	i32 volatile* counter;
} test_counter_task_t;

typedef struct test_nested_task_t {
	thread_pool_t* pool;
	task_group_t* group;
	i32 volatile* counter;
} test_nested_task_t;

typedef struct test_large_userdata_task_t {
	i32 volatile* counter;
	u8 bytes[512];
} test_large_userdata_task_t;

static void increment_counter_task(int logical_thread_index, void* userdata) {
	(void)logical_thread_index;
	test_counter_task_t* task = (test_counter_task_t*)userdata;
	atomic_increment(task->counter);
}

static void nested_counter_task(int logical_thread_index, void* userdata) {
	(void)logical_thread_index;
	test_nested_task_t* task = (test_nested_task_t*)userdata;
	task_group_t child_group = task_group_create_child(task->group);
	test_counter_task_t child_task = {task->counter};
	REQUIRE(thread_pool_submit_task_to_group(task->pool, &child_group, increment_counter_task, &child_task, sizeof(child_task)));
	thread_pool_wait_for_group(task->pool, &child_group);
	atomic_increment(task->counter);
}

static void large_userdata_task(int logical_thread_index, void* userdata) {
	(void)logical_thread_index;
	test_large_userdata_task_t* task = (test_large_userdata_task_t*)userdata;
	for (i32 i = 0; i < COUNT(task->bytes); ++i) {
		REQUIRE(task->bytes[i] == (u8)(i & 0xFF));
	}
	atomic_increment(task->counter);
}

static void ensure_test_thread_memory(void) {
	if (!threadlocal_thread_memory) {
		init_global_system_info(false);
		init_thread_memory(&global_system_info);
	}
}

TEST_CASE("work queue executes submitted tasks on caller thread") {
	ensure_test_thread_memory();

	work_queue_t queue = work_queue_create("/slidescape_test_work_queue", 8);
	i32 volatile counter = 0;
	test_counter_task_t task = {&counter};

	CHECK(work_queue_submit_task(&queue, increment_counter_task, &task, sizeof(task)));
	CHECK(work_queue_submit_task(&queue, increment_counter_task, &task, sizeof(task)));
	CHECK(work_queue_submit_task(&queue, increment_counter_task, &task, sizeof(task)));

	while (work_queue_is_work_in_progress(&queue)) {
		CHECK(work_queue_do_work(&queue));
	}

	CHECK(counter == 3);
	CHECK(queue.start_count == 3);
	CHECK(queue.completion_count == 3);

	work_queue_destroy(&queue);
}

TEST_CASE("work queue helpers tolerate absent optional queue") {
	CHECK_FALSE(work_queue_do_work(NULL));
	CHECK_FALSE(work_queue_is_work_in_progress(NULL));
	CHECK_FALSE(work_queue_is_work_waiting_to_start(NULL));
}

TEST_CASE("work queue dynamically stores oversized userdata") {
	ensure_test_thread_memory();

	work_queue_t queue = work_queue_create("/slidescape_test_work_queue_large", 4);
	i32 volatile counter = 0;
	test_large_userdata_task_t task = {0};
	task.counter = &counter;
	for (i32 i = 0; i < COUNT(task.bytes); ++i) {
		task.bytes[i] = (u8)(i & 0xFF);
	}

	REQUIRE(sizeof(task) > sizeof(((work_queue_entry_t*)0)->userdata));
	REQUIRE(work_queue_submit_task(&queue, large_userdata_task, &task, sizeof(task)));
	while (work_queue_is_work_in_progress(&queue)) {
		CHECK(work_queue_do_work(&queue));
	}

	CHECK(counter == 1);
	work_queue_destroy(&queue);
}

TEST_CASE("completion queue posts and polls events") {
	completion_queue_t queue = completion_queue_create(4);
	i32 value = 42;

	REQUIRE(completion_queue_post(&queue, 1, &value, sizeof(value)));
	CHECK(completion_queue_has_events(&queue));

	completion_event_t event = {0};
	REQUIRE(completion_queue_poll(&queue, &event));
	CHECK(event.kind == 1);
	CHECK(*(i32*)event.userdata == 42);
	CHECK_FALSE(completion_queue_has_events(&queue));

	completion_queue_destroy(&queue);
}

TEST_CASE("thread pool runs work and can be destroyed") {
	ensure_test_thread_memory();

	init_global_system_info(false);
	system_info_t old_system_info = global_system_info;
	global_system_info.suggested_total_thread_count = 3;

	thread_pool_t pool = {};
	init_thread_pool(&pool, 32, true, false, NULL);

	i32 volatile counter = 0;
	test_counter_task_t task = {&counter};
	for (i32 i = 0; i < 16; ++i) {
		REQUIRE(work_queue_submit_task(pool.queue, increment_counter_task, &task, sizeof(task)));
	}

	thread_pool_wait_for_completion(&pool);
	CHECK(counter == 16);
	CHECK(pool.queue->completion_count == 16);

	thread_pool_destroy(&pool);
	CHECK(pool.initialized == 0);
	CHECK(pool.queue == NULL);
	CHECK(pool.high_priority_queue == NULL);

	global_system_info = old_system_info;
}

TEST_CASE("thread pool without high priority queue can be reused after destroy") {
	ensure_test_thread_memory();

	init_global_system_info(false);
	system_info_t old_system_info = global_system_info;
	global_system_info.suggested_total_thread_count = 2;

	for (i32 iteration = 0; iteration < 2; ++iteration) {
		thread_pool_t pool = {};
		init_thread_pool(&pool, 8, false, false, NULL);
		REQUIRE(pool.initialized);
		CHECK(pool.high_priority_queue == NULL);

		i32 volatile counter = 0;
		test_counter_task_t task = {&counter};
		REQUIRE(work_queue_submit_task(pool.queue, increment_counter_task, &task, sizeof(task)));
		thread_pool_wait_for_completion(&pool);
		CHECK(counter == 1);

		thread_pool_destroy(&pool);
		thread_pool_destroy(&pool);
		CHECK(pool.initialized == 0);
	}

	global_system_info = old_system_info;
}

TEST_CASE("thread pool helpers submit normal and high priority work") {
	ensure_test_thread_memory();

	init_global_system_info(false);
	system_info_t old_system_info = global_system_info;
	global_system_info.suggested_total_thread_count = 2;

	thread_pool_t pool = {};
	init_thread_pool(&pool, 8, true, false, NULL);

	i32 volatile counter = 0;
	test_counter_task_t task = {&counter};
	REQUIRE(thread_pool_submit_task(&pool, increment_counter_task, &task, sizeof(task)));
	REQUIRE(thread_pool_submit_high_priority_task(&pool, increment_counter_task, &task, sizeof(task)));

	while (thread_pool_is_work_in_progress(&pool)) {
		if (!thread_pool_do_work(&pool)) {
			platform_sleep(1);
		}
	}
	CHECK(counter == 2);
	CHECK(pool.queue->completion_count == 1);
	CHECK(pool.high_priority_queue->completion_count == 1);

	thread_pool_destroy(&pool);

	global_system_info = old_system_info;
}

TEST_CASE("task groups allow nested tasks to wait for child work") {
	ensure_test_thread_memory();

	init_global_system_info(false);
	system_info_t old_system_info = global_system_info;
	global_system_info.suggested_total_thread_count = 2;

	thread_pool_t pool = {};
	init_thread_pool(&pool, 8, true, false, NULL);

	task_group_t group = {0};
	i32 volatile counter = 0;
	test_nested_task_t task = {&pool, &group, &counter};
	REQUIRE(thread_pool_submit_task_to_group(&pool, &group, nested_counter_task, &task, sizeof(task)));
	thread_pool_wait_for_group(&pool, &group);

	CHECK(counter == 2);
	CHECK(task_group_is_complete(&group));

	thread_pool_destroy(&pool);

	global_system_info = old_system_info;
}

#include <atomic>
#include <thread>
#include <chrono>

TEST_CASE("work queue preserves every payload with concurrent producers and consumers across wraparound") {
	// Small queue, many transfers: exercise slot reuse while another consumer copies.
	work_queue_t queue = work_queue_create("/slidescape_test_queue_wrap", 31);
	const int producer_count = 4, consumer_count = 4, per_producer = 10000;
	const int total = producer_count * per_producer;
	std::atomic<int>* seen = new std::atomic<int>[total]();
	std::atomic<int> finished_producers(0), received(0), corrupt(0);
	std::atomic<bool> stop(false);
	struct payload_t { int id; u8 bytes[512]; };
	size_t payload_size = 0;
	SUBCASE("inline userdata") { payload_size = 124; }
	SUBCASE("heap userdata") { payload_size = sizeof(payload_t); }
	const int byte_count = (int)payload_size - sizeof(int);
	std::thread producers[producer_count], consumers[consumer_count];
	for (int c = 0; c < consumer_count; ++c) consumers[c] = std::thread([&]() {
		while (!stop.load()) {
			work_queue_entry_t entry = work_queue_get_next_entry(&queue);
			if (!entry.is_valid) { std::this_thread::yield(); continue; }
			payload_t payload;
			memcpy(&payload, entry.heap_userdata ? entry.heap_userdata : entry.userdata, payload_size);
			free(entry.heap_userdata);
			bool valid = entry.task_identifier == 123 && entry.userdata_size == payload_size &&
			             payload.id >= 0 && payload.id < total;
			for (int j = 0; valid && j < byte_count; ++j) valid = payload.bytes[j] == (u8)(payload.id + j);
			if (valid) ++seen[payload.id]; else ++corrupt;
			++received;
			work_queue_mark_entry_completed(&queue);
		}
	});
	for (int p = 0; p < producer_count; ++p) producers[p] = std::thread([&, p]() {
		for (int i = 0; i < per_producer && !stop.load(); ++i) {
			payload_t payload = {}; payload.id = p * per_producer + i;
			for (int j = 0; j < byte_count; ++j) payload.bytes[j] = (u8)(payload.id + j);
			while (!stop.load() && !work_queue_submit_notification(&queue, 123, &payload, payload_size)) std::this_thread::yield();
		}
		++finished_producers;
	});
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while ((finished_producers.load() < producer_count || received.load() < total) &&
	       std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
	stop = true;
	for (auto& producer : producers) producer.join();
	for (auto& consumer : consumers) consumer.join();
	CHECK(finished_producers.load() == producer_count);
	CHECK(received.load() == total);
	CHECK(corrupt.load() == 0);
	int missing_or_duplicate = 0;
	for (int i = 0; i < total; ++i) if (seen[i] != 1) ++missing_or_duplicate;
	CHECK(missing_or_duplicate == 0);
	delete[] seen;
	work_queue_destroy(&queue);
}

TEST_CASE("nested task groups publish results before stack groups are reused") {
	ensure_test_thread_memory();
	system_info_t old_system_info = global_system_info;
	global_system_info.suggested_total_thread_count = 4;
	thread_pool_t pool = {};
	init_thread_pool(&pool, 128, true, false, NULL);
	i32 volatile counter = 0;
	for (int round = 0; round < 100; ++round) {
		task_group_t group = {};
		test_nested_task_t task = {&pool, &group, &counter};
		for (int i = 0; i < 16; ++i) {
			REQUIRE(thread_pool_submit_task_to_group(&pool, &group, nested_counter_task, &task, sizeof(task)));
		}
		thread_pool_wait_for_group(&pool, &group);
		CHECK(counter == (round + 1) * 32);
	}
	thread_pool_destroy(&pool);
	global_system_info = old_system_info;
}

TEST_CASE("atomic helpers preserve updated values and strong compare exchange semantics") {
	volatile i32 value = 3;
	CHECK(atomic_increment(&value) == 4);
	CHECK(atomic_decrement(&value) == 3);
	CHECK(atomic_add(&value, -8) == -5);
	CHECK(atomic_subtract(&value, -2) == -3);
	CHECK_FALSE(atomic_compare_exchange(&value, 9, 7));
	CHECK(atomic_load_acquire(&value) == -3);
	CHECK(atomic_compare_exchange(&value, 9, -3));
	CHECK(atomic_load_acquire(&value) == 9);
}
