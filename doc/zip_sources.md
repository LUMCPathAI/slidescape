# Stored ZIP sources: first implementation

The shared source layer supports ordinary directories and ZIP/ZIP64 archives with
stored (method 0) members. Native MRXS is the first migrated image backend. The
viewer can open a `.zip` containing exactly one MRXS companion directory, including
nested directories. Discovery uses `Slidedat.ini`; a `.mrxs` thumbnail and an entry
point manifest are unnecessary. OpenSlide is never used for ZIP inputs.

Multiple MRXS slides currently produce an explicit unsupported-selection error.
A container is a namespace, not implicitly a study. DICOM grouping and selection
can later operate on the container root; multiple unrelated single-file slides
should have explicit selection/grouping rather than being assigned one study by
packaging alone. DICOM, TIFF and iSyntax ZIP opening are not yet integrated.

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
then read them concurrently. Stop readers before closing the namespace. MRXS owns
its namespace and closes it after outstanding tile work completes. The lower-level
ZIP index borrows its container; callers using that API own the container lifetime.

The namespace is a file-access abstraction, not an OS security sandbox. Ordinary
local directory access retains normal filesystem link resolution. A future archive
packager must explicitly reject or define source symlinks instead of following them
into an unintended preservation scope.

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

## Archive integration remaining

This change provides the shared reader/writer and migrates existing archive MRXS
adapter reads to bounded views. It does **not** yet add ZIP ingestion, change the
repository schema/identity policy, or migrate existing path-dependent recipes.

The next stage should stream the complete original bundle into a staged ZIP,
validate it, retain one immutable object hash, and publish only after finalization.
The shell should store enough member placements/payload offsets to serve frames
without mounting or parsing the ZIP. ZIP parsing belongs to ingestion and shell
rebuild; opening a remote slide should fetch its shell, and subsequent tile reads
should fetch only their resolved byte ranges. Staging, durable installation and
publication remain archive responsibilities, outside the shared ZIP writer.

The writer never publishes or repairs partial output. Sink errors and short source
reads fail the operation; the caller must discard staged output. No in-place ZIP
append or update API is exposed.

## Verification

Tests use small in-memory archives and tiny temporary files. A callback-backed
virtual archive covers a member larger than 4 GiB and another member whose offset
is beyond 4 GiB, without allocating or writing that payload. Tests also cover
CRC-free mounting versus explicit validation, concurrent reads, nested/implicit
namespaces, empty/hidden/UTF-8 members, malformed headers and unsafe paths, failed
writes, and native MRXS discovery/decoding through the viewer's image-loading path.

Verified on macOS arm64: Debug and RelWithDebInfo viewer/archive builds; the full
unit suite (126 cases, 428,578 assertions; two fixture skips); the archive loopback
integration test; and 14 targeted AddressSanitizer cases. Python's `zipfile`
independently verified writer output, and the reader validated Python-generated
streaming ZIP32 and ZIP64 inputs. The 65,536-member boundary test is entirely in
memory. Windows and Linux branches were reviewed but not built in this session.
