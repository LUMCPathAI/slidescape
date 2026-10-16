#include "tile_buffer_pool.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

using benchmark_clock = std::chrono::steady_clock;

static double elapsed_ms(benchmark_clock::time_point start) {
	return std::chrono::duration<double, std::milli>(benchmark_clock::now() - start).count();
}

static void benchmark_mixed_small_allocations() {
	constexpr size_t slot_count = 4096;
	constexpr size_t operation_count = 2000000;
	void* slots[slot_count] = {};
	u32 random = 0x12345678;

	auto start = benchmark_clock::now();
	for (size_t operation = 0; operation < operation_count; ++operation) {
		random = random * 1664525u + 1013904223u;
		size_t slot = random & (slot_count - 1);
		free(slots[slot]);
		size_t size = 16 + ((random >> 12) & 0xfff);
		slots[slot] = malloc(size);
		if (!slots[slot]) std::abort();
		((u8*)slots[slot])[0] = (u8)random;
	}
	for (void* pointer : slots) free(pointer);

	std::printf("mixed-small:       %8.2f ms  (%zu operations)\n",
	            elapsed_ms(start), operation_count);
}

typedef u8* (*tile_acquire_func_t)(size_t size);
typedef void (*tile_release_func_t)(u8* buffer, size_t size);

static u8* malloc_tile(size_t size) {
	return (u8*)malloc(size);
}

static void free_tile(u8* buffer, size_t size) {
	(void)size;
	free(buffer);
}

static double benchmark_tile_handoff(const char* name, size_t tile_size, size_t operation_count,
		tile_acquire_func_t acquire, tile_release_func_t release) {
	/* The common local-file policy permits 16 decoded tiles in flight. */
	constexpr size_t queue_capacity = 16;
	u8* queue[queue_capacity] = {};
	std::atomic<size_t> write_position = 0;
	std::atomic<size_t> read_position = 0;
	std::atomic<bool> start_threads = false;

	std::thread producer([&] {
		while (!start_threads.load(std::memory_order_acquire)) std::this_thread::yield();
		for (size_t i = 0; i < operation_count; ++i) {
			while (i - read_position.load(std::memory_order_acquire) >= queue_capacity) {
				std::this_thread::yield();
			}
			u8* buffer = acquire(tile_size);
			if (!buffer) std::abort();
			buffer[0] = (u8)i;
			buffer[tile_size - 1] = (u8)(i >> 8);
			queue[i & (queue_capacity - 1)] = buffer;
			write_position.store(i + 1, std::memory_order_release);
		}
	});
	std::thread consumer([&] {
		while (!start_threads.load(std::memory_order_acquire)) std::this_thread::yield();
		for (size_t i = 0; i < operation_count; ++i) {
			while (write_position.load(std::memory_order_acquire) <= i) {
				std::this_thread::yield();
			}
			u8* buffer = queue[i & (queue_capacity - 1)];
			release(buffer, tile_size);
			read_position.store(i + 1, std::memory_order_release);
		}
	});

	auto start = benchmark_clock::now();
	start_threads.store(true, std::memory_order_release);
	producer.join();
	consumer.join();
	double milliseconds = elapsed_ms(start);
	std::printf("%-18s %8.2f ms  (%zu x %zu KiB)\n", name, milliseconds,
	            operation_count, tile_size / 1024);
	return milliseconds;
}

int main() {
#if SLIDESCAPE_USE_MIMALLOC
	std::puts("allocator: mimalloc static override");
#else
	std::puts("allocator: system CRT");
#endif
	benchmark_mixed_small_allocations();

	constexpr size_t tile_256k = 256 * 1024;
	constexpr size_t tile_1m = 1024 * 1024;
	benchmark_tile_handoff("tile-256k malloc:", tile_256k, 30000, malloc_tile, free_tile);
	tile_buffer_pool_trim();
	benchmark_tile_handoff("tile-256k pool:", tile_256k, 30000,
	                       tile_buffer_acquire, tile_buffer_release);
	tile_buffer_pool_trim();
	benchmark_tile_handoff("tile-1m malloc:", tile_1m, 10000, malloc_tile, free_tile);
	tile_buffer_pool_trim();
	benchmark_tile_handoff("tile-1m pool:", tile_1m, 10000,
	                       tile_buffer_acquire, tile_buffer_release);

	tile_buffer_pool_stats_t stats = tile_buffer_pool_get_stats();
	std::printf("pool totals: allocations=%llu reuses=%llu releases=%llu frees=%llu retained=%llu bytes\n",
	            (unsigned long long)stats.system_allocations,
	            (unsigned long long)stats.reuse_count,
	            (unsigned long long)stats.release_count,
	            (unsigned long long)stats.system_frees,
	            (unsigned long long)stats.retained_bytes);
	tile_buffer_pool_trim();
	return 0;
}
