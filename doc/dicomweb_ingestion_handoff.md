# DICOMweb WSI And Pathology Archive Handoff

Status: interoperable viewer prototype; early read-only archive service
Last reviewed: 2026-07-26

This document records current decisions, implementation state, and the next useful
slices. Historical experiments and detailed protocol notes were removed because they
duplicated tests, code, and the DICOM standard. The focused archive contract lives in
`doc/pathology_archive_architecture.md`.

## Direction

The direction remains sound:

1. DICOMweb is the external clinical image API.
2. Orthanc and dcm4chee are independent interoperability targets, not the intended
   production storage architecture.
3. Already-converted DICOM WSI is the first end-to-end fixture and regression oracle.
4. The production archive is pathology-first: immutable vendor-native bundles may
   remain the canonical pixel source.
5. A persisted, versioned virtual-DICOM representation defines stable UIDs, metadata,
   dimensions, frame order, transfer syntax, and provenance.
6. QIDO reads a publication index. Request handlers never discover studies by
   scanning source bundles.
7. WADO metadata and frames are deterministic products of the published
   representation. Native compressed bytes may pass through only when their DICOM
   semantics are exact; otherwise an adapter reconstructs or transcodes them.
8. Materialized Part 10 DICOM is an export/interchange/cache representation, not a
   required second canonical copy.
9. Native format adapters and conversion run outside the public request process or in
   tightly bounded workers.
10. The archive remains a standalone service boundary and does not depend on GUI or
    renderer state.

The hybrid representation is valuable for more than storage savings: it preserves
vendor metadata, avoids mandatory whole-slide retiling before publication, supports
native research access, and permits format-specific indexing. It also means the
product owns publication consistency, authorization, audit, retention, migration,
backup, and disaster recovery; a generic PACS previously supplied much of that.

## Non-Negotiable Representation Invariants

For a published representation version, all of the following form one logical DICOM
object and must remain stable:

- Study, Series, Concatenation, and SOP Instance UIDs;
- governed patient, study, specimen, container, acquisition, and optical-path data;
- image flavor, dimensions, tiling organization, focal planes, and frame order;
- pixel spacing, origin/orientation, overlap reconstruction, and total pixel matrix;
- photometric interpretation, ICC/color policy, transfer syntax, and lossy history;
- the bytes returned for each frame, or the deterministic rule producing them;
- source bundle digest, adapter identity/version, metadata version, and provenance.

Changing semantics requires a new representation version and, where DICOM identity
semantics require it, new UIDs. “Virtual” must never mean approximate or generated
opportunistically inside a request handler.

## Implemented Viewer Work

The branch currently provides:

- recursive local DICOM WSI discovery and multi-instance pyramid assembly;
- JPEG Baseline and vendored OpenJPEG JPEG 2000 decoding in the local tile path;
- a DICOMweb protocol module for DICOM JSON metadata, QIDO references, WADO paths,
  multipart native-frame extraction, UID validation, and query encoding;
- an asynchronous archive browser for studies, series, and instances with filtering,
  pagination, cancellation generations, stale-result handling, and dynamic storage;
- a DICOMweb image backend that maps explicit per-frame matrix positions to viewer
  tiles and uses the shared tile cache;
- bounded response headers and bodies, incremental HTTP parsing, separate post-connect
  write/idle/total deadlines, credential redaction, URL/request validation, and
  connection reuse with retry;
- pinned Orthanc and dcm4chee development harnesses plus captured/public IDC
  regressions;
- unit coverage for metadata variants, scoped QIDO results, multipart bounds,
  truncation, close-delimited dcm4chee responses, URL/query injection, credential
  redaction, and ownership cleanup.

The viewer is a strong interoperability prototype, not production-complete DICOMweb.
The most important remaining transport gap is that DNS resolution, TCP connect, and
TLS handshake happen before the cancellable/total request deadline. A stalled connect
can therefore outlive UI cancellation. Fix this before claiming M1 completion.

The current remote frame request advertises JPEG Baseline and JPEG 2000 transfer
syntaxes that the viewer can decode. The DICOMweb tile-cache policy submits batches
of up to four already-scheduled tiles without adding a coalescing delay. Tile workers
combine frames from the same instance into RetrieveFrames and retry failed parts
individually. A regression test pins that batching policy. Uncompressed
bulk-data negotiation, servers that decline all advertised compressed syntaxes,
additional multipart variants, optical paths, focal planes, multidimensional sparse tiling,
concatenations, associated images, and diagnostic color policy remain acceptance
work.

## Implemented Archive Work

`slidescape_archive` is an opt-in standalone target with:

- `init`, `check`, and loopback-only `serve` commands;
- a vendored SQLite local profile with an explicit application/schema identity,
  strict foreign-keyed durable-model tables, WAL journaling, and read-only validation;
- staged SHA-256 object import with no-overwrite content-addressed placement,
  duplicate verification, and idempotent SQLite registration;
- atomic Part 10 bundle publication in SQLite and SQLite-backed reconstruction of
  the in-memory QIDO/provider index;
- virtual-shell binary format v2 with an aligned section directory, stable
  source-digest references, inline optical-path/ICC data, compact regular or explicit
  frame semantics, fixed-stride or delta-offset payload lookup, and validated
  single-instance and aggregate Part 10 writers/loaders;
- SQLite schema v5 publication of one aggregate shell per initial Study/Series
  logical-slide group, with a durable shell-directory index and individual DICOMweb
  addressing for every SOP Instance;
- dedicated content-addressed fast-tier shell storage, canonical retained Part 10
  generation recipes, deterministic representation fingerprints, and atomic SQLite
  publication of shell/QIDO/provider/frame/audit rows;
- inline SQLite storage for canonical bundle descriptors and regeneration recipes,
  preserving ordinary object hashes and references without making placement part of
  provenance and without a separate application-level size or placement threshold;
- a deliberately constrained `brightfield_p480.mrxs` native-provider spike using
  Slidescape's built-in MRXS reader (never OpenSlide): ingestion checksums the
  `.mrxs`, `Slidedat.ini`, `Index.dat`, and every declared data file, then publishes
  one `TILED_SPARSE` VOLUME instance per native pyramid level in one aggregate shell;
  compact row runs enumerate only occupied native positions, and frame WADO passes
  their standalone JPEG tiles through without decoding;
- development-only external Part 10 ingestion from an arbitrary file or directory.
  Canonical absolute paths are stored in SQLite instead of copying bytes or creating
  filesystem links under `objects/`. Sources are checksummed during ingestion;
  provider-cache admission checks availability and size, so moving, changing, or
  deleting the external source invalidates retrieval;
- SQLite index reconstruction from published shell representations, a bounded
  aggregate-shell cache shared by every instance in the logical slide, and a bounded
  per-instance provider cache. The shared entry owns the validated shell bytes and,
  for MRXS, the open native data-file handles; the instance entries project metadata
  and retain only instance-specific source state;
- a versioned repository layout separating immutable objects, bundle provenance,
  representations, durable published state, disposable derived data, and staging;
- shared DICOM UID validation and bounded source/virtual-instance C contracts;
- route classification for QIDO, WADO metadata/frames, and STOW;
- bounded study-, series-, and instance-level QIDO DICOM JSON with exact UID filters,
  `limit`, and `offset`;
- bounded series metadata WADO projected from one validated aggregate shell in its
  persisted instance-directory order, returning one complete DICOM JSON dataset per
  published instance;
- a provider boundary whose first adapter loads the virtual shell once, verifies
  published identities and source digests, projects instance metadata, and reads
  original encoded frame bytes by compact payload-map offset;
- archive request dispatch through Slidescape's worker pool and per-thread temporary
  arenas, with bounded queue admission and graceful draining;
- startup validation that published series have a study and a complete contiguous
  instance directory, and that instances have a series;
- read and write socket deadlines and Unix broken-pipe protection;
- focused repository, routing, index, shell, and client-parser tests.

The flat TSV compatibility snapshots and the superseded staged JSON manifest path
have been removed. Current limitations are:

- the index is loaded once at startup and does not provide live publication;
- duplicate checks are simple and not designed for cloud-scale cardinality;
- the current manifest describes provider ownership, regular versus explicit frame
  geometry, slide coordinates, dimension counts/defaults, pixel spacing, and an
  optional ICC digest; compact explicit spatial/dimension maps can now be generated
  and validated. Aggregate recipe v7 retains the ordered source-object set and
  grouping policy. The regenerated shell retains patient/study scalar identifiers
  and full patient names; optical-path identity, description, illumination codes,
  filter bands, objectives, DICOM Color Space, validated inline ICC bytes, and
  field-origin masks; container/specimen identity and description; and a typed,
  nested specimen-preparation tree preserving concept names, CODE, TEXT, NUM with
  units, UIDREF, DATETIME, DATE, TIME, and PNAME values. Metadata WADO reconstructs
  the corresponding DICOM content-item sequences;
- pre-v7 Part 10 development publications are no longer accepted. Policy v7 removes
  the flat 16-byte-per-frame explicit payload records. Encapsulated frames use one
  64-bit base plus parallel 32-bit frame-start delta and correction arrays, with one
  explicit final-frame size; all other frame sizes are derived from the next delta.
  Unordered, overlapping, or otherwise non-representable inputs use parallel
  absolute-offset and size arrays. Development repositories must be recreated and
  ingested again;
  automated representation upgrades remain future work;
- staging is not crash-durable and is not safe as a multi-process transaction;
- virtual-shell representation publication is atomic after its immutable inputs are
  stored. Missing or corrupt Part 10 shells are lazily regenerated after validating
  the retained recipe and source digest, and are restored atomically only when the
  regenerated bytes match the published digest; multi-adapter job orchestration remains;
- Part 10 instance metadata and ordered multi-frame WADO are implemented with
  provider-facing representation selection. Stored compressed frames are passed
  through, while unsigned 8-bit RGB frames can be generated losslessly as Explicit VR
  Little Endian `application/octet-stream`. Series metadata uses the same shell
  projection and verifies complete ordered index coverage. Broader photometric
  generation and STOW remain.
- development `--reference` Part 10 sources are hashed during ingestion, but ordinary
  cache admission now checks only target availability and byte size rather than
  rehashing an entire WSI. This removes a large cold-open cost while preserving the
  explicitly non-durable reference-mode contract. Owned immutable objects and shell
  regeneration continue to use their published SHA-256 identities. Reference mode
  stores an absolute external path in SQLite; it creates neither an object payload
  nor a symbolic link under `objects/`;
- the MRXS spike deliberately retains an absolute local source path in its inline
  recipe and does not copy the approximately 4.7 GB fixture into archive ownership.
  Its descriptor proves what was admitted, but request-time reads trust that external
  location rather than rehashing the entire bundle. This is suitable for format-
  neutrality testing only, not cloud durability or mutation-safe clinical serving;
- the pinned MRXS v5 profile accepts only brightfield JPEG pyramids with known pixel
  spacing, no tile overlap, one optical path/focal plane, and at most 16 levels.
  Associated images, fluorescence, arbitrary overlap reconstruction, specimen
  governance, and diagnostic ICC/color policy remain. It publishes native occupancy
  as `TILED_SPARSE`; missing grid positions are not DICOM frames and cannot be
  requested. Each occupied frame now carries a shell-native data-file index, byte
  offset, and JPEG length. Normal serving opens the retained `.dat` files from the
  admitted bundle descriptor and no longer parses `Slidedat.ini` or `Index.dat`;

The pinned Orthanc harness is a protocol oracle, but not a safe performance oracle
for the private 487 MB / 38,478-frame UMCU top-level instance. Orthanc 1.12.11 with
the DICOMweb 1.23 plugin is OOM-killed in an 8 GB Docker VM while retrieving even one
native frame from that instance; its smaller pyramid instances support the same
single- and four-frame requests. Keep Orthanc's local WADO-RS loader sequential and
use a moderate-size public WSI for interactive frame tests. Do not add provider-
specific Orthanc tile routes to the viewer to conceal this limitation.

New protocol handlers should continue to depend on index/provider interfaces rather
than embedding SQLite access in HTTP routing.

## Review Findings

The recent branch work is on a good path and generally improves boundaries and input
hardening. The following findings should guide further work:

- Keeping DICOM parsing/protocol code separate from GUI code is working well.
- Sharing UID validation and the local DICOM decoder with the archive is preferable
  to building a second DICOM stack.
- Dynamic browser storage and bounded HTTP parsing remove earlier prototype limits.
- The provider-specific IDC URL translation is acceptably isolated; it must remain
  outside core QIDO/WADO behavior.
- Tile-cache scheduling, locking, result ownership, read leases, and shutdown
  lifetimes are implemented. Accounting and eviction remain viewer performance work,
  not a reason to reintroduce the standalone streamer; see
  `doc/tile_streamer_handoff.md`.
- Part 10 now uses the transactional SQLite/object-store boundary; virtual-shell
  representation publication must join that contract rather than rely on manifest
  renames.
- The current virtual-instance struct is useful validation scaffolding but is too
  small to be declared the canonical virtual-DICOM schema.

No issue found requires abandoning the pathology-first archive direction. The next
read-only protocol slices can proceed while the high-impact storage and governance
decisions are made explicitly.

## Next Implementation Slices

Proceed in this order:

The shared tile-cache locking/lifetime prerequisite and bounded four-frame DICOMweb
batching are complete. Continue with:

1. Close the pinned MRXS v5 semantic profile before making it durable: validate the
   admitted JPEG profile, photometric/subsampling agreement, lossy-compression and
   derivation metadata, ICC/color policy, missing-versus-corrupt tile handling, and
   sampled native/virtual/materialized parity.
2. Exercise idempotent replay, conflicting SOP Instance UIDs, interrupted aggregate
   publication, and failing repair jobs. Reject unsupported published adapter
   versions at `check`/startup rather than exposing them through QIDO and failing
   later with a generic metadata conflict. Treat cross-process cloud repair as
   durable background work rather than a responsibility of public request handlers.
3. Replace the MRXS development locator with transactional owned source-bundle
   ingestion and object-based native locators.
4. Add STOW and source-bundle admission on top of the hardened publication contract.
5. Broaden native profiles and the viewer's normalized WSI model only after color,
   associated-image, multidimensional sparse tiling, concatenation, optical-path, focal-plane,
   overlap, specimen-governance, and scanner-variant policies are explicit.

The aggregate and series-metadata slices are implemented with deterministic
Study/Series grouping and persisted shell-directory ordering. Typed
specimen-preparation content is retained without flattening its step or parent-child
structure. Policy v7 promotes ten canonically equal patient/study/series attributes
plus equal container/specimen/preparation sections, while retaining per-instance
overrides for the remaining attributes. Series metadata reuses the aggregate cache,
rejects incomplete or inconsistent index spans, and projects every instance as a
complete independent DICOM dataset. Governed acquisition grouping remains future
schema work. Continue Part 10 response-parity tests throughout.

The real local UMCU fixture now exercises a 38,478-frame `TILED_SPARSE` Part 10 map,
including build, fixed-record serialization, parsing, and comparison with every
per-frame Plane Position (Slide). This fixture also exposed and fixed a parser issue
that could append a phantom optical-path item while processing sequence delimiters.
The remaining fixture gap is a multi-optical-path/multi-focal specimen.

The durable aggregate, binary format, and ingestion boundary are specified in
`virtual_dicom_shell_v0_1.md`. Further implementation should publish that shell
through transactional repository interfaces instead of expanding the handwritten
staging JSON into a de facto database.

## Acceptance Gates

### Viewer transport

- DNS/connect/TLS handshake is bounded and cancellable.
- Certificate and hostname validation remain enabled.
- Header/body/multipart limits and truncated-response tests remain.
- Credentials and patient-bearing URLs are absent from logs.

### Viewer WSI behavior

- Multi-instance pyramids, missing levels, non-raster frame order, and concatenations
  are correct.
- JPEG Baseline and JPEG 2000 behavior is explicit and tested.
- Color, optical path, focal plane, multidimensional sparse tiling, and associated images are either
  supported or rejected with a clear profile error.

### Owned archive

- Standard clients use standard paths without native-format knowledge.
- QIDO never scans source bundles.
- WADO metadata and frames agree exactly with the published representation.
- Publication is atomic across source, governed metadata, representations, and index.
- Duplicate identities, partial bundles, checksum mismatch, and adapter failure are
  deterministic conflicts/failures, not partial publication.
- Native adapters cannot crash or exhaust the public service process.
- Orthanc and dcm4chee remain independent protocol oracles.

## Delivery Roadmap

The older handoff had useful acceptance contracts but too much completed history.
This compact roadmap keeps the specification value while leaving implementation
detail in tests and release notes.

### A1: Read-only Part 10 archive profile

Deliverables: published study/series/instance QIDO, instance/series metadata WADO,
native single- and multi-frame WADO, pagination, and a Part 10 provider.

Acceptance: the public WSI fixture is discoverable and viewable in Slidescape through
Slidescape Archive; stored transfer syntax, frame bytes, frame order, and metadata
match the source; malformed routes, missing objects, UID mismatch, and oversized
responses fail closed; the same black-box profile runs against Orthanc and dcm4chee.

### A2: Durable DICOM ingest and atomic publication

Deliverables: content-addressed object ingest, complete governed metadata and
representation schema, transactional index, idempotent publication, conflict/audit
records, and recovery tests.

Acceptance: identical replay is idempotent; UID reuse with different content is a
conflict; process failure at every ingest step exposes either the old publication or
the complete new one, never a partial study; QIDO/WADO never reference missing data.

### A3: First native-format virtual DICOM provider

Deliverables: one pinned scanner-format adapter, immutable bundle manifest, virtual
WSI shell, deterministic frame map, metadata WADO, frame WADO, and optional Part 10
materialization.

Acceptance: native, virtual-DICOM, and materialized-DICOM geometry and sampled pixels
agree within the declared color/loss policy; repeated serving is byte/semantics stable;
adapter failure cannot crash or exhaust the API process.

### A4: Clinical security and operations

Deliverables: OIDC/OAuth authorization, tenant/policy boundaries, audit trail,
retention/legal hold, monitoring, backups, restore drills, migrations, and gateway
deployment.

Acceptance: threat-model and access-control tests pass; every publication/retrieval is
attributable as required; backup restore meets agreed RPO/RTO; rolling schema and
adapter upgrades preserve published representation semantics.

### Client gates (parallel)

Before calling the viewer production-ready: bound/cancel DNS, connect, and TLS;
complete transfer-syntax negotiation beyond native JPEG/JPEG 2000 pass-through;
broaden sparse tiling beyond one optical path/focal plane, and define support or
rejection for concatenations and associated
images, optical paths, focal planes, and diagnostic color; retain cross-server parser
and credential-redaction tests.

The remote viewer admits single-path/single-focal `TILED_FULL` and `TILED_SPARSE`
VOLUME instances. Sparse per-frame positions are expanded into the existing dense
X/Y tile map, with missing tiles marked empty and therefore never requested. Before
multidimensional acceptance, converge
the local Part 10 and DICOMweb paths on one normalized WSI model that can represent
concatenation parts, sparse positions, optical paths, and focal planes without
collisions. Add associated-image presentation, ICC/color handling, uncompressed
fallback decoding, explicit capability diagnostics, and archive-backed annotation
versioning as separate client slices.

## Decisions Requiring Product/Stakeholder Input

These should be resolved before production publication/ingest, but they do not block
the current read-only DICOM provider work:

- organizational DICOM UID root and UID allocation/versioning policy;
- authoritative systems for patient, accession, specimen, container, and stain data;
- diagnostic color-management and visual-fidelity acceptance criteria;
- first supported scanner formats, versions, brightfield/fluorescence modes, optical
  paths, and focal planes;
- durable object store, transactional index, deployment topology, and migration plan;
- authentication/authorization model, tenant boundaries, audit requirements, and
  gateway responsibilities;
- retention, legal hold, deletion, backup, and disaster-recovery policy;
- required DICOMweb surface beyond the viewer subset: complete-instance WADO,
  rendered retrieval, STOW, deletion, and lifecycle operations.

## Development Rules

- Keep private WSI outside tracked fixtures; use manifest entries that skip cleanly.
- Treat MRXS as a bundle and checksum every source object.
- Use Slidescape's native MRXS reconstruction as the production source reader;
  wsidicomizer, Orthanc WSI, and OpenSlide are comparison implementations.
- Preserve 64-bit sizes and bound every untrusted length before allocation.
- Do not weaken TLS or log credentials to make a test pass.
- Report exact server versions, transfer syntaxes, fixtures, and platforms tested.
- Build `slidescape_archive` only with `BUILD_SLIDESCAPE_ARCHIVE=ON`; it is not part of
  routine viewer health checks yet.

## Primary References

- DICOM PS3.18 Web Services
- DICOM Whole Slide Imaging
- IHE PaLM Digital Pathology Image Acquisition
- Orthanc DICOMweb and WSI documentation
- dcm4chee Archive 5 and dcm4che DICOMweb examples
- NCI Imaging Data Commons DICOMweb documentation
- OpenSlide MIRAX format documentation
- wsidicomizer documentation
