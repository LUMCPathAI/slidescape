# Tile Cache Handoff

Status: generation-aware viewer scheduling is integrated into the shared tile cache;
DICOMweb again submits bounded four-tile batches, while thread safety, broader
deterministic tests, and eviction remain unfinished.
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

## Important Correctness Gap

`tile_cache_t` contains a mutex, but most cache state reads and writes currently do
not take it. Viewer, worker, and read-region paths access fields such as
`decode_in_flight`, `demand_mask`, pins, residency, generation, and pixel pointers
concurrently. Before adding eviction or relying on stronger request-merging
guarantees, define and enforce a locking contract.

Recommended contract:

- cache state transitions and ownership changes occur under `cache->lock`;
- no slow I/O, decode, renderer call, allocation, or queue wait occurs while locked;
- APIs returning a raw `tile_cache_tile_t*` are internal-only or replaced by
  snapshot/transition functions;
- pixel pointers stay valid through an explicit pin held by the caller;
- renderer texture destruction remains on the renderer-owning thread;
- cache destruction first prevents new work and drains/joins outstanding work.

The current behavior may appear reliable on common platforms, but the unsynchronized
fields are not a safe C concurrency contract.

## Remaining Work

In priority order:

1. Extend deterministic cache tests beyond the DICOMweb batch-policy regression to
   cover duplicate suppression, generation changes, hard-demand joining, pins, stale
   completion, and result draining.
2. Implement and test the cache locking/lifetime contract.
3. Simplify `load_tile_task_t`; it still carries compatibility fields that overlap
   cache state.
4. Add CPU/GPU byte accounting and configurable budgets.
5. Add conservative LRU eviction. Never evict pinned or in-flight resources, and
   destroy GPU resources on the renderer thread.
6. Add view/generation/backend-aware eviction scoring only after the simple policy is
   measured.
7. Decide separately whether adapting iSyntax to the shared scheduler is worthwhile.

The first integration regression should cover `image_read_region()` joining a tile
already requested by the viewer. The first concurrency regression should force a
camera-generation change while a decode completes and a read-region pin is acquired.

## Target Invariants

- The cache owns generic tile request state and CPU/GPU residency.
- Workers publish results; they do not transfer ownership through caller-private
  completion queues.
- Viewer demand is soft and generation-based.
- Read-region, export, and registration demand is hard while pinned.
- Only viewer-only work may be discarded as stale.
- CPU and GPU eviction are independent and budget-driven.
- Tile state and resource lifetimes are race-free and explicit.
