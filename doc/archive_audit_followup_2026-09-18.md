# Archive audit implementation follow-up — 18 September 2026

Applied the immediate correctness, performance, resource-bounding, and readability
changes from the [audit](archive_audit_2026-09-18.md). No existing archive repository
was migrated or modified by the verification runs.

## Applied

- **Metadata:** shell v3 retains ORIGINAL/DERIVED and NONE/RESAMPLED provenance and
  full-grid organization. JSON now includes High Bit, Pixel Representation, Planar
  Configuration, origin/orientation, concatenation fields, and sparse X/Y/Z and
  optical-path identification. Z fields are named in micrometres to match their
  existing DICOM values. Admission checks the four-value WSI ImageType and rejects
  full multi-plane grids until their focal spacing can be retained.
- **Publication:** each Study/Series group has its own canonical bundle, independent
  of the discovery batch. A→A+B and exact replay succeed. Bundles remain STAGED until
  all prepared publications commit in one transaction; a failure leaves no partially
  visible new representations. Existing series replacement/completion stays explicit
  future work.
- **Decoders:** JPEG and JPEG 2000 validate expected dimensions and pixel-output bounds
  before image decoding. JPEG checks allocation failures and releases output on
  libjpeg errors. TIFF deserialization no longer writes a string terminator beyond a
  binary JPEG-table allocation.
- **Frame performance:** native payload lookup no longer scans every frame size;
  metadata is projected only for metadata requests; only delta-offset maps are
  expanded. Cold loads are coalesced. Cache byte reservations cover both resident and
  incoming shells/directories, and native source handles have a separate budget.
- **HTTP:** multipart frames are sent from their existing buffers after validation,
  without a second complete body allocation. WADO has bounded response admission.
  Socket operations use both an idle timeout and an absolute accept-time deadline.
  Queue depth is reduced; provider failures reach stderr; health reports the actual
  repository schema and cache bytes. Accept quality parsing uses integer thousandths
  and rejects NaN, exponents, signs, and excessive precision.
- **Index:** startup reads one database snapshot, replaces quadratic duplicate scans
  with sorted validation, and uses binary UID/series lookup. Additional generated
  frame representations no longer duplicate instance rows. Publication and serving
  share the supported transfer-syntax mapping.
- **Native MRXS:** admission inspects all occupied JPEG headers for the supported
  baseline YCbCr profile, dimensions, sampling, and tables. It no longer invents an
  sRGB profile. New native UIDs derive from bundle membership instead of adapter
  version. Ingest avoids reopening the adapter. Development tile helpers now use
  explicit zero-based native grid indices, distinct from published frame numbering.
- **Code organization:** extracted DICOM JSON projection into `archive_metadata.*`,
  replaced heuristic response sizes with exact bounded counting, consolidated file
  hashing, removed the redundant Part 10 publication wrapper, and renamed shared
  publication records. Source roles, instance flags, string slots, descriptor ordinals,
  and version policies now have named definitions. Updated the architecture docs.

## Compatibility and practical limits

New output uses shell v3, Part 10 recipe/adapter v10, and MRXS recipe/adapter v6.
Repository layout v2 and SQLite schema v5 are unchanged. Intact shell v2 publications
remain readable, including MRXS v5. Legacy ImageType provenance is omitted because it
was not retained. Only current recipes can regenerate missing Part 10 shells.

Use a **fresh development repository** to reingest existing sources with the corrected
metadata and profiles. There is no automatic migration or replacement. MRXS changes
its identity derivation once at this transition; subsequent adapter changes can keep
source identities stable. Reference-only ownership and path-dependent recipes remain
limitations until storage placements are separated from source identity.

Default bounds are 512 MiB for shared shells, 64 MiB for expanded frame directories,
256 retained MRXS data handles, 256 MiB per metadata/multipart response, and 512 MiB
across admitted WADO responses. These do not constitute a total RSS cap. Decoder
working memory, parser/repair allocations, worker arenas, and the eager index remain
additional consumers. The 30-second I/O deadline does not cancel an active decoder,
filesystem operation, or regeneration. Admission checks JPEG headers, not every
compressed pixel or complete DICOM conformance.

## Verification

- macOS arm64 Debug builds: archive, viewer, and tests.
- Full CTest: 112 doctest cases and 428,264 assertions passed, two explicitly skipped
  cases; the separate synthetic CLI/HTTP integration test also passed.
- AddressSanitizer: targeted archive/shell/native-adapter tests and the synthetic
  CLI/HTTP integration test passed.
- Synthetic integration covers A→A+B, replay, invalid-payload visibility, atomic
  rollback, multiple representations, concurrent cold requests, metadata semantics,
  ordered/repeated multipart frames, and negotiation failures. It requires no private
  WSI fixtures.
- A slow client sending one byte per second was disconnected after 30.1 seconds;
  health remained available afterward.
- The original Debug microbenchmark at one million native payload records fell from
  approximately 254 ms to 0.007 ms per 100 lookups. This measures payload lookup only,
  not network or whole-slide throughput.
- Windows/Linux builds and sustained resource-pressure tests were not run.

## Next implementation work

The remaining large changes still need separate designs: a compact/live query index;
logical members and storage placements; owned source retention; S3 range reads;
store-only ZIP64 preservation/export; and native tiled TIFF/BigTIFF admission. Linux
VPS packaging, graceful shutdown, cancellation, metrics, backup/restore, migration and
replacement policy, deep integrity verification, and durable repair/admission jobs
also remain. A controlled Linux deployment trial can precede the storage backends.
