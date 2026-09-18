# Slidescape Pathology Archive Architecture

Status: executable service boundary established; read-only study/series/instance
QIDO-RS and fast-tier virtual-shell publication are implemented. The local repository
creates and validates the SQLite durable model. Part 10 ingest publishes governed
shell rows through SQLite, and serving reads those published representation rows and
digest-validated shells.

The `slidescape_archive` target replaces the defunct TIFF byte-range `slideserver`.
It is a standalone process and must not depend on the Slidescape GUI, renderer, or
interactive viewer state.

## Repository Contract

`slidescape_archive init REPOSITORY` creates a versioned repository:

```text
REPOSITORY/
  repository.json   schema and publication model
  archive.sqlite3   transactional metadata, publication, and QIDO authority
  objects/           immutable content-addressed source payloads
  shells/            immutable content-addressed fast-tier virtual DICOM shells
  staging/           transient files used by atomic object installation
```

The directories describe ownership boundaries, not a commitment to a local-filesystem
production backend. Object storage and a transactional database may implement the same
interfaces later. Data under `staging/` is never canonical and must not be
discoverable through QIDO or retrievable through WADO.

Publication will be one atomic state transition covering the immutable source-bundle
manifest, governed metadata version, virtual-DICOM representation manifests, and QIDO
index records. Moving bytes between directories is not itself publication.

## Service Boundary

Build with `-DBUILD_SLIDESCAPE_ARCHIVE=ON`. The initial process supports:

```text
slidescape_archive init REPOSITORY
slidescape_archive check REPOSITORY
slidescape_archive ingest REPOSITORY
slidescape_archive serve REPOSITORY [--port PORT]
```

For the current local development workflow, `ingest` recursively discovers `.dcm`
Part 10 sources placed below `objects/`, imports immutable copies into the
content-addressed store, validates their WSI identities and dimensions, and publishes
the bundle and its query projection. The initial Part 10 grouping policy treats one
Study/Series pair as one logical slide and rejects reuse of a Series Instance UID
across studies. In deterministic SOP Instance UID order, it derives one canonical
retained recipe and one multi-instance shell per group under the dedicated fast
`shells/` tier, then publishes the representation and all QIDO, provider, frame,
publication, and outbox rows in one SQLite transaction. It fails before
representation publication if an object is malformed, lacks required WSI metadata,
or reuses a SOP Instance UID for different content. This remains a convenience path
for interactive testing, not the eventual STOW/source-bundle ingestion pipeline.

The HTTP listener is deliberately restricted to loopback IPv4 (`127.0.0.1`) and
IPv6 (`::1`, when available). `GET /health` validates
that the process opened a compatible repository. Study-, series-, and instance-level
QIDO-RS reads a validated in-memory projection loaded from published SQLite shell
representations at process startup and returns bounded DICOM JSON. Queries support
`limit`, `offset`, and the exact UID matching key for their query level. Published
virtual DICOM shells provide instance metadata and bounded payload lookup for
negotiated frames through WADO-RS without request-time Part 10 metadata parsing.
Series metadata is one bounded DICOM JSON array projected directly from the complete
aggregate shell in its persisted instance-directory order; the provider validates
that the published index covers every shell instance exactly once before responding.
RetrieveFrames accepts an ordered comma-separated list of up to 64 frames and emits
one multipart item per frame with its canonical `Content-Location`; response bodies
are bounded to 256 MiB. Frame representation selection supports multipart root types,
media wildcards, quality and specificity ordering, exact or wildcard transfer syntax,
and required/missing `Accept` handling. Providers advertise stored and generated
representations before frame I/O. The Part 10 provider passes through native JPEG
Baseline/JPEG 2000 and uncompressed frames; encapsulated unsigned 8-bit RGB can also
be generated losslessly as interleaved RGB in Explicit VR Little Endian
`application/octet-stream`. Broader photometric transcoding and STOW-RS remain
incomplete.

Representation selection is a testable provider-facing contract rather than a
Boolean check in the HTTP handler. The selector rejects missing or wholly
unacceptable `Accept` headers, ranks supported media-type/transfer-syntax alternatives
using quality and specificity, distinguishes stored pass-through from generated
representations, and returns the selected representation metadata used by both the
multipart headers and frame provider. Generated uncompressed output is deliberately
limited to a profile whose RGB semantics are unambiguous. JPEG Baseline generation
for clients that cannot decode stored JPEG 2000 still requires an explicit lossy
quality/color policy and is not a prerequisite for extending the schema.

The former TSV compatibility index has been removed. SQLite schema v5 stores the Part
10 query projection and provider locator. Startup loads only `PUBLISHED`
representations, using one SQLite read snapshot for studies, series, and instances. Protocol handlers continue to consume the in-memory
index/provider interface, so a future live database-backed query implementation does
not need to alter HTTP routing.

TLS termination, OIDC/OAuth policy, rate limiting, and externally reachable bindings
belong at a reviewed gateway boundary. The current loopback listener is development
scaffolding, not a production HTTP implementation.

## Representation Contracts

The early staged JSON manifest implementation has been removed. SQLite publication,
retained generation recipes, and the virtual shell are now the only representation
contracts. Source formats and future adapters must enter through those boundaries
rather than introducing a parallel manifest authority.

`virtual_dicom_shell_v0_1.md` defines the durable representation contract, including
aggregate ownership, identity/versioning, optical-path and color policy, provider
behavior, publication state, and worked Part 10 and MRXS-native examples. Binary
format v3 has an aligned section directory plus single-instance and aggregate
Part 10 writers/loaders. The aggregate writer combines validated instance sections,
remaps their source/string/section references, and retains independent payload maps
inside one logical-slide object. Transactional representation publication records the
shell as canonical metadata and the exact aggregate generation recipe as provenance.
The index and byte provider select instances by their persisted shell-directory index.
Broader governed attributes remain; identical optical-path/ICC sections are shared.

`archive_frame_map.*` defines the explicit-map encoding embedded by shell v1. It has a
64-byte little-endian header followed by fixed 32-byte records in DICOM frame order.
Each record stores the tile's one-based total-matrix row/column, optional physical
X/Y/Z offsets, and zero-based optical-path/focal-plane assignments. The fixed record
size and declared 4096-record block size permit bounded O(1) lookup without a database
row per frame. The builder follows the standard implicit order for `TILED_FULL`
(column, row, depth, optical path, including concatenation offsets); for sparse images
it requires and preserves every per-frame Plane Position (Slide) and Optical Path
Identification value. It refuses inconsistent counts, unknown optical paths, invalid
positions, malformed lengths, unsupported versions, and out-of-range assignments.

Frame maps live inside the immutable content-addressed shell. Each Study/Series group
has its own canonical source bundle, so replaying it in a larger discovery batch does
not change its identity. Registration stages bundles; one final transaction publishes
all prepared bundles, representations, provider/index rows, and outbox events together.
Failed admission may leave unreferenced objects or staged bundles, which remain
invisible to QIDO. Exact replay is idempotent. Adding instances to an already published
series or replacing it is deliberately rejected until explicit versioning exists.

The current shell writer is v3, the Part 10 recipe/adapter is v10, and the native MRXS
recipe/adapter is v6. Binary v2 remains readable. Its absent ImageType provenance is
omitted from JSON rather than inferred. Only current Part 10 recipes can regenerate
shells; older intact shells remain usable, but applying the corrected metadata/profile
to old publications requires a fresh development repository. No in-place migration or
silent publication replacement is performed. Repository layout v2 and SQLite schema
v5 are unchanged. Bundle registration adapter versions (Part 10: 1, MRXS: 2) describe
the descriptor/member contract, independently of shell-generation versions.

MRXS remains reference-only. Every admitted tile must have a standalone baseline
8-bit YCbCr JPEG header, matching dimensions, supported sampling, and required tables
within the first 64 KiB. Admission does not decode every pixel or claim an sRGB profile.
New native UIDs derive from canonical source membership, independently of adapter
version. The current descriptor still includes only known referenced files and does
not preserve an arbitrary original directory tree; owned ZIP64 preservation remains
separate work.

DICOM JSON projection is isolated in `archive_metadata.*`. It preserves ImageType,
pixel interpretation, origin/orientation, concatenation, and sparse physical/optical
positions. Exact size counting accounts for escaped strings and inline ICC data.
TILED_FULL multi-focal-plane admission is rejected until focal spacing is retained.

The provider shares immutable shells and coalesces cold loads with striped locks.
Metadata is serialized on demand and never needed to serve a frame. Only delta-offset
maps are expanded; fixed-stride and explicit-offset maps remain directly addressable.
Native payload lookup validates layout and the requested record after full validation
at shell opening. Defaults cap shared shell bytes at 512 MiB, expanded directories at
64 MiB, and retained native payload handles at 256; reservations include cold loads.
HTTP bounds each metadata/multipart response to 256 MiB and reserves at most 512 MiB
across admitted WADO responses. Multipart sending does not duplicate frame buffers.
These are resource-specific bounds, not a total process RSS ceiling: decoder working
memory, the eager index, parser/repair allocations, and worker arenas also consume RAM.

The blocking loopback server has a 64-entry worker queue, 5-second socket idle timeout,
and 30-second absolute I/O deadline measured from accept (including queue time).
Deadlines are checked between frame operations; they do not interrupt an in-progress
filesystem read, decoder, or shell regeneration. `/health` includes shell and expanded
directory byte accounting. Graceful shutdown, cancellation, deployment configuration,
and production request metrics remain future work. `check` validates layout/schema;
it is not a deep integrity or retrievability scan.

For verification, build `slidescape_archive` and `slidescape_tests`, then run CTest.
When Python 3 is available, CTest also runs `tests/test_archive_integration.py`, which
uses only the standard library, generated public synthetic DICOM, temporary repositories,
and a localhost listener. No private fixtures are needed for that integration test.
Run `python3 tests/test_archive_integration.py ./slidescape_archive --slow` to also
exercise the absolute deadline against a client sending one byte per second.

## Intended Production Storage Model

The filesystem repository is an executable model of ownership boundaries. A likely
production implementation is:

- S3-compatible object storage for immutable source objects, bundle descriptors,
  Part 10 objects, ICC profiles, compact frame maps, and derived artifacts;
- PostgreSQL (or another database with equivalent transactions and indexing) for
  governed metadata, identities, representation versions, publication state, QIDO
  search fields, provider locators, provenance, audit references, and retention state;
- a gateway for TLS, identity, coarse request policy, and rate limiting;
- stateless archive API processes plus isolated adapter/validation workers;
- an outbox/job table for durable post-commit cache work and notifications.

For development, tests, small installations, and an external-dependency-free local
mode, the same interfaces should also support SQLite plus ordinary filesystem object
storage. This is a deployment profile, not a second information model: migrations,
publication invariants, provider locators, and conformance tests should be shared with
the PostgreSQL/object-store profile. SQLite publication uses one writer transaction;
filesystem objects use content-addressed names, atomic rename, and the same rule that
only fully validated objects may become discoverable.

The first local schema is now created as `archive.sqlite3` during repository
initialization. It establishes strict, foreign-keyed tables for immutable objects,
bundles, metadata versions, shell representations, QIDO projections, instance
providers, promised frame representations, publications, and outbox work. The
database carries an application ID and explicit schema version and is opened in WAL
mode. Repository `open` and `check` validate it read-only. Schema v5 intentionally has
no migration path during this development phase: delete and recreate the repository
with `slidescape_archive init REPOSITORY`. Part 10 ingest registers immutable
members and marks their bundle `PUBLISHED` in one writer transaction; lookup admits
only members of published bundles.

The local immutable-object writer is also implemented. File and in-memory inputs are
copied to exclusive files under `staging/` while SHA-256 and byte size are computed.
After the staged file is flushed, it is installed without overwrite at
`objects/sha256/HH/FULL_DIGEST`, the containing directory is made durable where the
platform supports directory synchronization, and the object record is inserted in a
single SQLite writer transaction. Replays return the existing object identity.
Existing digest paths are rehashed before reuse; a digest path containing different
bytes is an integrity failure. An object successfully installed before a database
failure may remain unreferenced, preserving the required safe asymmetry.

For the local profile, canonical control objects are retained inline in
SQLite. Generation recipes and bundle descriptors keep ordinary object rows, SHA-256
identities, media types, retention states, and foreign-key references, but store
their bytes in `inline_data` instead of allocating one filesystem inode each.
Storage placement is not provenance and is not part of object identity. There is no
application-level inline threshold: SQLite's configured blob/transaction limits and
available memory are the effective bounds. Source DICOM objects and virtual shells
always remain external payloads.

The database should contain records conceptually equivalent to:

```text
object(id, sha256, size, media_type, storage_key | inline_data, retention_state)
bundle(id, source_format, bundle_digest, adapter, adapter_version, state)
bundle_object(bundle_id, object_id, logical_name, role)
metadata_version(id, governed_metadata_object_id, schema_version, provenance)
representation(id, version, bundle_id, metadata_version_id, state, manifest_object_id)
study / series / instance(representation_id, DICOM identities and indexed QIDO fields)
instance_provider(instance_id, kind, source_object_id, frame_map_object_id, parameters)
publication(id, representation_id, committed_at, actor, policy_version)
outbox(id, publication_id, operation, state)
```

This is a logical model, not a requirement to expose each item as a separate table.
Frequently searched QIDO fields should be typed/indexed columns; the complete governed
DICOM attribute set can also be retained as a versioned canonical document. Avoid a
row per WSI frame: a slide can contain millions of frames, and most requests need a
small lookup into a regular or sparse frame map. Store regular geometry as parameters
and sparse/irregular mappings as immutable, versioned, chunked objects with bounded
random access.

### Publication Transaction

1. Upload content-addressed objects. Uploading alone never makes data discoverable.
2. Parse and validate the bundle in an isolated worker.
3. Create the governed metadata version, representation manifest, stable UIDs,
   provider locator, and frame-map object in a staged state.
4. Verify referenced objects, DICOM invariants, frame samples, and policy decisions.
5. In one database transaction, insert/update all QIDO rows, mark the representation
   published, and append its publication/audit/outbox records.
6. API queries select only committed published versions. Derived cache work may occur
   after commit because derived data is never canonical.

Object storage and the database cannot generally share a distributed transaction.
The safe asymmetry is therefore “unreferenced immutable objects may exist; published
rows may never reference missing or unvalidated objects.” Garbage collection can
remove unreferenced objects after a quarantine period.

### Virtual DICOM Shell

A virtual-DICOM shell is an immutable representation manifest plus governed metadata
and provider references. It should contain or reference:

- stable DICOM identities and representation version;
- the exact standard attributes returned by metadata WADO;
- total matrix, tile size, organization type, frame count/order, optical paths, focal
  planes, geometry, specimen/container, color, and lossy-compression history;
- source bundle/object digests and adapter name/version/configuration;
- a provider kind and immutable locator;
- regular frame-order parameters or a compact frame-map object;
- the promised transfer syntax and whether frames are pass-through or generated;
- hashes/version keys for materialized metadata, frames, and Part 10 caches.

Metadata WADO should project this shell deterministically; it should not reopen and
reinterpret a vendor slide on every request. Frame WADO resolves the provider locator
and frame map, then either returns exact native compressed bytes or invokes the pinned
adapter/transcoder version described by the shell. A semantics-changing adapter or
metadata update creates a new representation version rather than silently changing a
published object.

### Hot Instance and Streaming Runtime

The request/response protocol must not force a request-local implementation. API
processes should keep a bounded cache of opened provider instances, parsed frame maps,
file handles, metadata JSON, decoder/adapter state, and recently requested compressed
frames. Entries need explicit memory/file-handle budgets, reference counts, idle/LRU
eviction, representation-version keys, and invalidation on publication changes.

Treat interactive viewing like a latency-sensitive game/content-streaming workload:
avoid parsing and opening files on the frame hot path, coalesce duplicate requests,
prioritize visible frames over prefetch, cancel obsolete viewport work, preserve byte
buffers through the send path where practical, and instrument queueing/open/read/send
latencies separately. DICOMweb remains the wire contract, not the performance model.
The development Part 10 adapter now keeps up to 16 validated instances in a
process-lifetime LRU. Entries own the parsed frame map, file handle, and serialized
metadata; reference counts prevent eviction during concurrent frame reads. Cold loads
coalesce missing/corrupt-shell repair within one process: recovery validates the
retained source and canonical recipe, requires an exact published shell digest, and
atomically restores the fast-tier object. Cross-process repair coalescing and telemetry
remain deployment follow-ups. The fixed entry count is only an initial file-handle
budget. The viewer client submits up to four already-scheduled DICOMweb tiles per
worker task without a coalescing delay, then combines consecutive, non-stale requests
for one instance into a multi-frame WADO request. It maps returned parts using their `Content-Location`
frame number, accepts response order as a compatibility fallback, and retries missing
or invalid parts individually. Its console reports cumulative tile request counts,
batch-size distribution, decoded frames, request failures, per-reason fallbacks, and
average request latency on the first request, first multi-frame request, every 16
requests, and whenever a batch fallback occurs.

### Pyramid Awareness

Keeping HTTP routes independent of viewer internals does not require the archive to be
ignorant of a logical slide pyramid. The governed representation should group related
VOLUME instances, resolutions, concatenations, associated LABEL/OVERVIEW/THUMBNAIL
images, optical paths, and focal planes into one queryable slide representation. That
model enables validation, prefetch hints, cache locality, and future slide-oriented
APIs while preserving standards-correct instance/frame DICOMweb endpoints.

The open design question is where pyramid selection policy lives. The current viewer
can assemble/select DICOM instances client-side, which keeps DICOMweb conventional.
The archive may later expose an additional Slidescape-native slide manifest or streaming
endpoint for lower-latency clients; it should derive from the same immutable virtual
DICOM shell rather than introduce a competing source of truth.

### Logical-Slide Aggregate

One published representation and one virtual shell describe one logical slide, not
one SOP Instance. SQLite schema v5 and the Part 10 publisher now implement this
aggregate with a persisted shell-directory index per DICOM instance. The initial
grouping identity is the Series Instance UID within its Study; governed
container/specimen/acquisition grouping must replace or extend that provisional
policy before native adapters are added.

The aggregate shell uses DICOM scopes rather than an arbitrary "first instance"
inheritance rule:

- provenance and source-object references belong to the representation;
- governed patient, study, container, specimen, and preparation facts are shared at
  their real scope;
- series records group their projected instances;
- optical paths, ICC profiles, strings, and immutable attribute sets may be shared
  explicitly;
- each instance retains its identity, image flavor, geometry, dimensions, provider,
  frame map, payload map, and any typed attribute overrides.

Metadata WADO must reconstruct a complete independent DICOM dataset for every
instance. Shared values are promoted only after canonical typed comparison; a
conflict in a governed shared field is rejected or resolved by explicit policy.
Metadata must never implicitly mean "copy whatever instance zero contained."

The physical layout remains analogous to a TIFF IFD directory, but TIFF is only a
layout analogy. DICOM study/series/instance semantics and provider addressing remain
authoritative.

A source bundle is not automatically one logical slide. It may contain multiple
studies, containers, specimens, or acquisitions. Ingestion therefore needs a
versioned grouping decision based on governed slide/container/specimen identity and
DICOM relationships, plus an explicit override for ambiguous fixtures. The grouping
decision and its provenance participate in the representation fingerprint.

### Shell Availability And Repair

The content-addressed shell is the serving authority for a published representation,
but it is reproducible from retained immutable inputs and a pinned recipe. Local
request processes may coalesce bounded cache repair as a development convenience.
In a cloud deployment, durable object storage is responsible for shell availability
and a background outbox/job worker performs reconstruction and telemetry. Public
request handlers should not become the primary cross-process repair orchestrator.

## Component Direction

The first format-neutrality spike is implemented for the private P480 brightfield
fixture. It uses the built-in MRXS reader through the generic shell, publication,
index, and provider contracts; HTTP routing contains no MRXS behavior. The profile is
intentionally narrow: regular non-overlapping JPEG tiles, known spacing, one optical
path/focal plane, native JPEG pass-through, and one `TILED_SPARSE` VOLUME instance
per native level. Only occupied native tile positions become DICOM frames.
The development recipe currently points to the original
absolute path even though ingestion checksums every bundle member. This locator is
not an owned, mutation-safe cloud source.

External development ingestion uses one command for all detected formats:
`ingest REPOSITORY --source PATH --reference`. Part 10 canonical absolute paths are
stored in SQLite rather than copied into `objects/`; MRXS retains its bundle path in
the inline provider recipe. Provider-cache admission avoids rehashing the WSI. MRXS
currently requires `--reference` because owned multi-file bundle import is not yet
implemented. This is explicitly external ownership, not durable archive storage.

The viewer tile-cache locking/lifetime prerequisite and bounded four-frame DICOMweb
batching are complete. The next implementation slices are:

1. Close the MRXS v2 DICOM-semantic profile: JPEG admission validation,
   photometric/subsampling agreement, lossy and derivation metadata, ICC/color
   policy, absent-versus-corrupt tile behavior, and pixel/geometry parity tests.
2. Exercise idempotent replay, conflicting UIDs, interrupted aggregate publication,
   failing repair jobs, and fail-fast adapter-version validation. Keep request-local
   coalescing bounded; implement cross-process cloud repair through durable jobs and
   telemetry.
3. Import native source bundles into durable object ownership and replace filesystem
   paths in provider recipes with logical object locators.
4. Add STOW and source-bundle admission only after publication failure semantics are
   established.
5. Expand pinned native profiles and the normalized viewer WSI model after associated
   images, sparse tiling, concatenations, optical paths/focal planes, overlap
   reconstruction, scanner variants, specimen governance, and color policy have
   acceptance tests.

All protocol handlers depend on repository/index/provider interfaces. They must not
encode filesystem paths, MRXS details, or GUI types into the DICOMweb layer.
