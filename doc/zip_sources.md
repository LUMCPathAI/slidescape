# Stored ZIP sources and owned MRXS ingestion

The viewer opens stored ZIP/ZIP64 bundles through the native MRXS, DICOM, and TIFF
backends, without extraction or OpenSlide. No entry-point manifest is needed:

- MRXS discovery uses `Slidedat.ini`, including in nested directories. The sibling
  `.mrxs` thumbnail is optional. MRXS discovery takes precedence over its image sidecars.
- DICOM discovery starts at the ZIP root, recursively recognizes Part 10 headers,
  and keeps the existing series grouping and multi-slide selector. Member extensions
  are not required; selecting another slide reopens the same container and member.
  Annotation sidecar names include the series UID so slides do not share annotations.
- A single TIFF/BigTIFF member is recognized by its header and opened directly.
  Multiple TIFFs, multiple MRXS slides, and mixed DICOM/TIFF containers are rejected
  pending a general slide-selection policy. iSyntax and simple-image ZIP loading
  are not yet supported.

## Shared interfaces

- `source_io.h`: a sized byte source and bounded file views. Exact `read_at` calls
  distinguish invalid ranges from I/O failures. No shared seek position, tile
  extraction, or per-read allocation in the POSIX path. Local Windows reads use
  buffered overlapped I/O, supporting unaligned member offsets.
- `source_zip.h`: a validated, immutable member index, direct member views, an
  explicit content-validation operation, and a sequential stored-ZIP64 writer.
- `source_fs.h`: directory/ZIP namespaces with relative file opening, stat and
  immediate-child enumeration. ZIP directory entries may be implicit. Local
  enumeration includes hidden files and empty directories.

Namespaces own their handles and indexes. Open member views on the loading thread;
then read them concurrently. Stop readers before closing the namespace. Image
backends own their namespace and close it after outstanding tile work completes.
Local TIFF and DICOM files also use bounded positional views, without closing and
reopening the file between metadata parsing and tile access. The lower-level
ZIP index borrows its container; callers using that API own the container lifetime.

The namespace is a file-access abstraction, not an OS security sandbox. Ordinary
local directory access retains normal filesystem link resolution. The archive packager rejects source
symlinks and Windows reparse points. Sources must remain stable during capture; this
is not a transactional filesystem snapshot.

## ZIP profile and preservation

Opening validates headers, descriptors, member ranges and paths, but does not scan
member payloads or calculate CRCs. Member tile reads go directly to the blob:

```
blob offset = member data offset + offset within member
```

Content CRC validation is an explicit ingestion/deep-check operation. ZIP's member
CRCs are format metadata, not separate archive identities. The intended archive
identity is one SHA-256 of the immutable complete ZIP. The writer's output callback
allows that digest to be calculated while bytes are written. Persistent member
SHA-256 hashes are not required. Repacking produces a different archive object;
unchanged identity across arbitrary repackaging is not a requirement.

The current writer preserves member bytes, UTF-8 relative names, empty files and
explicit empty directories. It uses a fixed ZIP timestamp; original timestamps,
permissions, ACLs and links are not part of this initial preservation contract.
It accepts an explicit complete member list: the caller must include sidecars and
other original files, not only files required to render the image.

The reader accepts ordinary stored ZIPs and ZIP64, including signed/unsigned and
32/64-bit data descriptors. It rejects compression, encryption, multi-volume
archives, Unix special file/link entries, traversal, duplicate paths, ASCII case
collisions and file/directory conflicts. Names must be valid UTF-8 and follow the
portable relative-path profile (no backslashes, drive names, Windows reserved
characters or device names). Paths are limited to 511 UTF-8 bytes. Unicode names
are preserved without normalization; Unicode-equivalent/case-folding collisions
on a particular export filesystem need an export-time policy.

Indexing is capped at one million members, with separate 64 MiB bounds for miniz's
working allocations and the copied member/range index. Names consume that index
budget too, so the effective entry limit can be lower. Content validation uses a
64 KiB buffer. Neither viewer opening nor tile reads hash the complete blob.

Miniz's full amalgamation replaces the old inflate-only build source. Its ZIP
header validator assumes an archive-wide data-descriptor width, which rejects
some valid mixed-width ZIP64 archives (including its own streaming output). The
adapter therefore checks the local header and descriptor against each central
entry itself. Miniz still parses the ZIP directory and writes the container.

## Archive ingestion

Build with `BUILD_SLIDESCAPE_ARCHIVE=ON`. Owned MRXS ingestion now accepts a thumbnail,
its companion directory alone, or an already-created stored ZIP:

```sh
./slidescape_archive init data/archive_repo
./slidescape_archive ingest data/archive_repo --source /slides/example.mrxs
./slidescape_archive ingest data/archive_repo --source /slides/example
./slidescape_archive ingest data/archive_repo --source /slides/example.zip
```

For a directory/thumbnail, capture includes the entire companion tree, hidden
sidecars, empty directories, and the optional sibling thumbnail. Files are sorted
by relative path. The writer streams into an exclusive staged file while computing
one SHA-256, flushes it durably, then validates the captured ZIP structure and every
member CRC. Capture keeps one input data handle open at a time and bounds its member
list to 64 MiB. An existing ZIP is imported byte-for-byte and fully validated from
the owned copy. Neither route creates persistent per-member content hashes.

The ZIP hash is the bundle identity and the input to generated DICOM UIDs. Admission
uses the same supported JPEG MRXS profile as reference ingestion. Only complete,
validated output can be published. Failures can leave an unreferenced immutable
object for future garbage collection, but never a partial publication.

MRXS ZIP recipe v7 contains the blob hash and size, with no external paths. Its shell
stores absolute blob offsets for native JPEG frames. Metadata requests open only the
shell; the ZIP blob handle opens lazily on the first tile request. Frame reads are
bounded direct ranges, with no ZIP mounting, directory parsing, or complete hashing.
Missing/corrupt shells rebuild from the retained recipe and hash-verified ZIP.
Repository/schema versions are unchanged, and legacy MRXS v5/v6 reference publications
remain readable. Re-ingesting legacy sources as owned ZIPs gives them new identities;
there is no silent replacement or migration.

`--reference` remains a development option for ordinary `.mrxs` and Part 10 paths.
It is not accepted for ZIP ingestion. The archive currently admits **MRXS ZIPs only**;
DICOM ZIP publication and native TIFF archive admission are subsequent adapter work.
Viewer support is broader than archive admission. S3 range storage and a source-download
endpoint are also still pending; the retained ZIP itself already preserves the original
member bytes and tree for standard ZIP tools to restore.

The writer never publishes or repairs partial output. Sink errors and short source
reads fail the operation; staged output is discarded. No in-place ZIP append or
update API is exposed.

## Verification

Tests use small in-memory archives and tiny temporary files. A callback-backed
virtual archive covers a member larger than 4 GiB and another member whose offset
is beyond 4 GiB, without allocating or writing that payload. Tests also cover
CRC-free mounting versus explicit validation, concurrent reads, nested/implicit
namespaces, empty/hidden/UTF-8 members, malformed headers and unsafe paths, failed
writes, and native MRXS discovery/decoding through the viewer's image-loading path.

Tests additionally cover owned MRXS capture (including hidden sidecars, an optional
thumbnail, and empty directories), idempotent ingestion, metadata with the blob
unavailable, frame reads with an unusable ZIP directory, shell regeneration, and
nested TIFF/DICOM ZIP tile decoding and DICOM slide selection. All new disk fixtures
are tiny; no multi-gigabyte files are written. Verified on macOS arm64: Debug and RelWithDebInfo viewer/archive builds; 131 passing
unit cases (428,789 assertions, two skips); the archive loopback integration test;
and 18 targeted AddressSanitizer cases (473 assertions). Windows and Linux still
need runtime verification.
