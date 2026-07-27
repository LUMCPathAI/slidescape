# Tile Cache Handoff

Status: generation-aware viewer scheduling is integrated into the shared tile cache;
DICOMweb submits bounded four-tile batches, cache state transitions and result
polling are locked, while atomic completion application, shutdown, integration tests,
and eviction remain unfinished.
Last reviewed: 2026-07-26

## Current Design

Generic non-iSyntax image backends now use one per-image `tile_cache_t` for request
state, decoded pixels, textures, and completion delivery. The viewer calls
`tile_cache_request_viewer_tiles()` with camera state; the cache computes visible
tiles, prioritizes them, applies a backend policy, and submits bounded work.

The useful behavior from the old standalone `tile_streamer` experiment has therefore
already been restored in the cache-owned model:

- viewer generations change with camera, crop, or zoom state;
- viewer-only work can be discarded when its generation becomes stale;
- hard read-region/export/registration demand and CPU/GPU pins prevent stale discard;
- Slide Score and DICOMweb use small batches and bounded in-flight work;
- remote TIFF and OpenSlide have more conservative policies;
- decoded results use a cache-native `tile_cache_result_t` queue;
- cached CPU-to-GPU uploads also travel through that per-image cache queue;
- `image_read_region()` pins CPU tiles, joins in-flight work, waits on cache state,
  copies cache-owned pixels, and then unpins.

Do not restore the removed standalone generic tile streamer. iSyntax still has its
own streamer and global completion path; that is an explicit exception, not the
model for other backends.

DICOMweb batching is intentionally different from delayed request coalescing. The
cache submits up to four tiles already selected in one scheduler tick, and the worker
combines only consecutive frames belonging to the same SOP Instance. It does not wait
for additional work. Missing or invalid multipart results retain the existing
single-frame fallback.

## Locking And Lifetime Status

Cache state reads, reservations, pin changes, residency changes, generation updates,
and completion-queue polling now take `cache->lock`. Raw tile-state lookup is private.
Completion polling is serialized because the underlying queue supports concurrent
producers but only one consumer cursor. Hard-demand pin types have independent
reference counts, and cached CPU pixels receive an upload pin from reservation until
the renderer has consumed them.

The contract is:

- cache state transitions and ownership changes occur under `cache->lock`;
- no slow I/O, decode, renderer call, allocation, or queue wait occurs while locked;
- raw `tile_cache_tile_t*` values remain internal;
- pixel pointers stay valid through an explicit pin held by the caller;
- renderer texture destruction remains on the renderer-owning thread;
- cache destruction first prevents new work and drains/joins outstanding work.

The principal remaining correctness gap is completion application. Viewer and
read-region consumers still perform "mark decode finished", CPU adoption, and GPU
upload transition as separate cache calls. Those must become one locked cache
transition that returns slow follow-up work to perform after unlocking. Cache creation
also remains lazy, and destruction does not yet mark the cache stopping before
draining workers, consumers, and deferred renderer uploads.

## Remaining Work

In priority order:

1. Add the first integration regression: `image_read_region()` joins a tile already
   requested by the viewer.
2. Replace split completion handling with one atomic cache transition. Add the
   generation-change/decode-completion/read-pin concurrency regression.
3. Make cache creation eager, add a stopping state, prevent new reservations, and
   drain workers, result consumers, and renderer-owned uploads before destruction.
4. Simplify `load_tile_task_t`; it still carries compatibility fields that overlap
   cache state.
5. Add CPU/GPU byte accounting and configurable budgets.
6. Add conservative LRU eviction. Never evict pinned or in-flight resources, and
   destroy GPU resources on the renderer thread.
7. Add view/generation/backend-aware eviction scoring only after the simple policy is
   measured.
8. Decide separately whether adapting iSyntax to the shared scheduler is worthwhile.

Deterministic tests now cover sequential and competing duplicate suppression,
generation staleness versus hard demand, overlapping hard-demand pin types, nested
CPU pins, cached-upload pinning, ordered result draining, and exact-once concurrent
result consumption.

## Target Invariants

- The cache owns generic tile request state and CPU/GPU residency.
- Workers publish results; they do not transfer ownership through caller-private
  completion queues.
- Viewer demand is soft and generation-based.
- Read-region, export, and registration demand is hard while pinned.
- Only viewer-only work may be discarded as stale.
- CPU and GPU eviction are independent and budget-driven.
- Tile state and resource lifetimes are race-free and explicit.
