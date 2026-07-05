# DICOMweb WSI Ingestion And Viewer Handoff

Status: proposed implementation plan  
Last updated: 2026-07-04

This document defines the first implementation path for adding DICOMweb whole-slide
imaging (WSI) support around Slidescape. It is intended to be both an architectural
reference and a handoff for future coding agents.

The deliberately narrow first objective is:

> Store an already-converted DICOM WSI file set in two independent DICOMweb
> archives, discover it through QIDO-RS, retrieve its metadata and frames through
> WADO-RS, and display it through a new Slidescape DICOMweb backend.

MRXS conversion is the next milestone, not part of the first one. The production
MRXS route should build on Slidescape's native MRXS parser, camera/overlap
reconstruction, and tile decoder. Existing converters remain valuable as independent
references and comparison tools, but the OpenSlide path is not the intended source
reader for Slidescape ingestion.

## Executive Decisions

The current recommendations are:

1. Use DICOMweb as the public clinical image API.
2. Start with already-converted DICOM WSI. This removes conversion and vendor-format
   interpretation from the first interoperability test.
3. Use Orthanc with its official DICOMweb plugin as the primary local reference
   server.
4. Use dcm4chee Archive 5 as the independent interoperability target.
5. Optionally use the NCI Imaging Data Commons (IDC) DICOMweb endpoint as a
   read-only external test against real pathology data.
6. Do not build a production DICOMweb server in Slidescape during this phase.
7. Do not implement virtual DICOM over MRXS in the initial design.
8. Build the MRXS source side of the converter on Slidescape's native backend.
   Implement DICOM writing behind a separate converter boundary.
9. Preserve source MRXS bundles and record their checksums even when conversion
   succeeds.
10. Make interoperability with both Orthanc and dcm4chee an acceptance criterion.
    Behaviour observed from only one server must not become the implicit protocol
    specification.
11. Use `wsidicomizer` and the Orthanc WSI Dicomizer as external comparison
    implementations, not as the production MRXS ingest dependency.

These are implementation decisions, not final product or procurement decisions.
Orthanc and dcm4chee are initially test fixtures and reference implementations.

## Implementation Progress

As of 2026-07-05:

- manifest-driven DICOM WSI metadata tests exist in `tests/test_fixtures.cpp`;
- representative local DICOM frame decoding is covered;
- optional fixture tags cover multi-slide directories and concatenations;
- the ignored `tests/fixtures/local_manifest.txt` can point at private DICOM data
  without publishing paths or files;
- local checks have exercised a single-instance RGB WSI, a directory containing
  three slides, and an extensionless ten-level DICOM WSI;
- the tracked local-manifest example documents the DICOM fixture tags;
- a pinned, loopback-only Orthanc DICOMweb development harness exists under
  `tests/dicomweb/orthanc/`;
- the harness provides repeatable health, STOW-RS, QIDO-RS, metadata, and native
  WADO-RS frame checks;
- the small RGB WSI and two companion instances from one UMCU WSI series have
  passed upload, discovery, metadata, and native frame retrieval against
  `orthancteam/orthanc:26.6.1`;
- Orthanc groups the two tested UMCU instances into the same study and series;
- the extensionless `I5_HE` fixture is readable by Slidescape but Orthanc rejects
  it because required Study, Series, or SOP Instance UIDs are absent; it is
  therefore useful as a tolerant-reader regression fixture, but is not currently
  a valid STOW-RS archive-ingestion fixture;
- no Slidescape DICOMweb transport client has been implemented yet.

This establishes the local-file reader as a regression oracle for the future
DICOMweb backend. Orthanc now covers the first half of M0; a pinned dcm4chee
configuration and cross-server conformance run remain.

## Why DICOM-Only Comes First

Starting with DICOM files separates three hard problems that would otherwise fail
simultaneously:

- DICOMweb transport and multipart parsing;
- DICOM WSI pyramid/frame interpretation;
- vendor WSI conversion and clinical metadata construction.

The first milestone can therefore answer:

- Can the archive accept the DICOM WSI instances?
- Can it index the required metadata?
- Can it return individual compressed frames efficiently?
- Can Slidescape construct the same pyramid from DICOM JSON metadata that it
  currently constructs from local DICOM files?
- Are server behaviours sufficiently similar to support one portable client?

Only after those questions are answered should MRXS conversion be put in front of
the same pipeline. Otherwise, a malformed conversion could be mistaken for a client
or server defect.

## Scope

### Phase 1 Scope

- Local containerized DICOMweb test archives.
- DICOM VL Whole Slide Microscopy Image Storage instances.
- STOW-RS upload of DICOM Part 10 instances.
- QIDO-RS study, series, and instance discovery.
- WADO-RS instance/series metadata retrieval in DICOM JSON.
- WADO-RS raw frame retrieval.
- Brightfield, single optical path, single focal plane WSI.
- `TILED_FULL` images first.
- JPEG Baseline frame decoding first, because Slidescape already has this decoder.
- Display through the existing image, tile cache, worker, and renderer paths.
- Automated cross-server conformance tests for the subset used by Slidescape.

### Phase 2 Scope

- MRXS bundle validation and conversion.
- Native MRXS parsing and reconstruction through `src/mrxs/`.
- DICOM WSI writing from native, non-overlapping logical output tiles.
- Conversion using an isolated worker process.
- Conversion fidelity and metadata validation.
- Retention and provenance for the original MRXS bundle.
- Resumable upload into a quarantine area.
- Publication to DICOMweb only after validation succeeds.

### Explicit Non-Goals

- Replacing the LIS.
- Diagnostic production deployment.
- Implementing a new PACS/archive.
- Supporting every DICOMweb transaction.
- Arbitrary rendered-region retrieval.
- Annotation storage or reporting workflows.
- Multiple optical paths, fluorescence, or multiple focal planes in the first slice.
- DICOM concatenations in the first slice.
- Sparse WSI in the first slice.
- Browser deployment of Slidescape.
- Internet exposure of the test archives.

## Existing Slidescape Starting Point

Slidescape already contains useful pieces, but none should be treated as a complete
DICOMweb implementation.

### Reusable Viewer Pieces

- `image_t` and `level_image_t` provide a backend-neutral pyramid representation.
- The tile cache owns request state, CPU pixels, GPU textures, and completion state.
- Tile workers already support latency-bound remote backends and batched work.
- The Slide Score backend demonstrates remote metadata acquisition and JPEG tile
  loading.
- The local DICOM reader builds a WSI pyramid and maps local tiles to DICOM frames.
- The native MRXS reader understands levels, overlap, physical resolution, camera
  placement, simple images, and the MRXS multi-file structure.

Relevant code:

- `src/core/slide_score.c`
- `src/core/viewer_io_file.cpp`
- `src/core/remote.c`
- `src/core/image.c`
- `src/core/tile_loader.c`
- `src/dicom/dicom.c`
- `src/dicom/dicom_wsi.c`
- `src/mrxs/mrxs.c`

### Current Limitations That Affect This Work

The current HTTP implementation:

- performs only a narrow GET request shape;
- has a fixed `Accept` header;
- discards response headers;
- buffers entire response bodies;
- has no MIME multipart parser;
- has no general POST/STOW support;
- has no OAuth/OIDC token lifecycle;
- configures TLS certificate verification as optional;
- does not abort on certificate verification failure;
- can log complete requests containing bearer tokens.

The local DICOM implementation:

- reads Part 10 files rather than DICOM JSON metadata;
- assumes local random-access file handles for frame data;
- does not yet support concatenations;
- rejects multiple volume instances with the same width;
- does not support multiple focal planes or optical paths as viewer dimensions;
- currently decodes only JPEG in the WSI tile path;
- requires further malformed-input and bounds hardening.

The old `src/server.c` target is a custom experimental TIFF byte-range server. It
is not a DICOMweb archive foundation and is outside the implementation path in this
document.

## Independent DICOMweb Implementations

Suitable implementations do exist and are readily available for local testing.
They are generally DICOM archives that can store pathology SOP classes, rather
than servers exclusively dedicated to pathology.

### Orthanc With DICOMweb Plugin

Role: primary development/reference server.

Advantages:

- lightweight and easy to run locally;
- official DICOMweb plugin implements QIDO-RS, WADO-RS, STOW-RS, and WADO-URI;
- official Docker images can enable the DICOMweb plugin;
- official WSI framework includes a DICOMizer and WSI viewer;
- explicit pathology/WSI history and documentation;
- easy inspection through Orthanc's native REST API while debugging.

Cautions:

- the documentation describes DICOMweb support as a basic/reference
  implementation and states that the full standard is not implemented;
- metadata query performance and indexing behaviour depend on configuration;
- frame decoding/transcoding may require suitable codec plugins;
- DICOMweb and WSI plugins have AGPL licensing implications if their code is
  modified or incorporated. Calling a standalone Orthanc service is a different
  licensing situation, but legal review is still required for a product.

Reference:

- [Orthanc DICOMweb plugin](https://orthanc.uclouvain.be/book/plugins/dicomweb.html)
- [Orthanc WSI framework](https://www.orthanc-server.com/static.php?page=wsi)
- [Orthanc Team Docker images](https://orthanc.uclouvain.be/book/users/docker-orthancteam.html)
- [Orthanc licensing guide](https://orthanc.uclouvain.be/book/faq/licensing.html)

### dcm4chee Archive 5

Role: independent enterprise-style interoperability target.

Advantages:

- separate Java implementation and codebase from Orthanc;
- broad DICOM archive and DICOMweb functionality;
- official examples for QIDO, WADO, STOW, and multipart retrieval;
- Docker deployment is documented;
- closer to the operational shape of a full PACS/VNA than the minimal Orthanc
  setup;
- can later exercise authentication, audit, storage policies, and integration.

Cautions:

- significantly heavier local deployment;
- configuration includes multiple services and is not ideal for the first
  debugging loop;
- support for our exact WSI media types, transfer syntaxes, and multi-frame request
  patterns must be proven by tests rather than assumed.

Reference:

- [dcm4chee Archive 5](https://github.com/dcm4che/dcm4chee-arc-light)
- [dcm4che DICOMweb examples](https://dcm4che.github.io/dicomweb/)

### NCI Imaging Data Commons

Role: optional public, read-only interoperability target.

Advantages:

- contains real brightfield and fluorescence DICOM WSI;
- uses standard DICOMweb retrieval;
- exercises data and server behaviour not produced by the local conversion path;
- is already used by the Slim pathology viewer and EZ-WSI tooling.

Cautions:

- not suitable for upload tests;
- public API authentication, quotas, and dataset versions are external concerns;
- tests must not depend on mutable public data without pinning identifiers and
  handling unavailability.

Reference:

- [IDC DICOMweb access](https://learn.canceridc.dev/data/downloading-data/dicomweb-access)
- [IDC projects and conversion tooling](https://github.com/ImagingDataCommons)

### Other Optional Implementations

DICOMcloud is an independent .NET DICOMweb implementation and may be useful as an
additional compatibility target. It is not initially required because two local
archives plus IDC provide sufficient implementation diversity. Project activity,
supported transfer syntaxes, and WSI performance should be reassessed before adding
it to CI.

## Reference Architecture

```text
                          +-----------------------+
                          | Existing LIS / orders |
                          +-----------+-----------+
                                      |
                                      | later: HL7 v2 / FHIR
                                      v
+-------------+          +------------+-------------+
| Scanner or  |--------->| Ingest coordinator       |
| lab gateway |          | identity + state machine |
+-------------+          +------+-------------------+
                               |
                  +------------+-------------+
                  |                          |
                  v                          v
          +-------+--------+         +-------+---------+
          | Quarantine     |         | Conversion      |
          | object storage |-------->| worker (MRXS)   |
          +-------+--------+         +-------+---------+
                  |                          |
                  +------------+-------------+
                               |
                               | validated DICOM Part 10
                               v
                    +----------+-----------+
                    | DICOMweb archive     |
                    | STOW/QIDO/WADO       |
                    +----------+-----------+
                               |
                               | QIDO + metadata + frames
                               v
                    +----------+-----------+
                    | Slidescape           |
                    | DICOMweb backend     |
                    +----------------------+
```

For Phase 1, the ingest coordinator, quarantine storage, conversion worker, and LIS
connection are omitted. Test DICOM instances are sent directly to the archive with
STOW-RS.

## Protocol Profile Required By Slidescape

Slidescape should initially implement a documented subset of DICOMweb rather than
claim broad DICOMweb conformance.

### Store

Store is used by the test harness and later by the ingest service, not initially by
the interactive viewer.

Required transaction:

```text
POST {base}/studies
Content-Type: multipart/related; type="application/dicom"
```

Requirements:

- accept one or more Part 10 instances;
- parse the STOW response and identify success/failure per SOP Instance;
- retry only failed or unacknowledged instances;
- treat duplicate SOP Instance UID plus identical content as idempotent success;
- treat duplicate UID plus different content as a conflict requiring intervention.

The exact duplicate-storage behaviour varies by archive and must be recorded in the
server test matrix.

### Search

Required QIDO transactions:

```text
GET {base}/studies
GET {base}/studies/{study}/series
GET {base}/studies/{study}/series/{series}/instances
Accept: application/dicom+json
```

The first viewer implementation may open a WSI using explicit Study and Series UIDs.
General patient-name search is not required for the first slice.

Minimum indexed/discoverable values:

- Study Instance UID `(0020,000D)`;
- Series Instance UID `(0020,000E)`;
- SOP Class UID `(0008,0016)`;
- SOP Instance UID `(0008,0018)`;
- Modality `(0008,0060)`, expected `SM`;
- Instance Number `(0020,0013)`;
- Number of Frames `(0028,0008)`;
- Image Type `(0008,0008)` where returned/supported.

The WSI SOP Class is:

```text
1.2.840.10008.5.1.4.1.1.77.1.6
```

### Metadata Retrieval

Required transactions:

```text
GET {base}/studies/{study}/series/{series}/metadata
GET {base}/studies/{study}/series/{series}/instances/{instance}/metadata
Accept: application/dicom+json
```

The client must parse the DICOM JSON model, including:

- scalar values and their VRs;
- sequences and nested items;
- `BulkDataURI`;
- absent optional attributes;
- numeric values represented according to DICOM JSON rules;
- empty values;
- unknown attributes without failing the whole instance.

Metadata needed for the first WSI profile includes:

- Rows and Columns;
- Total Pixel Matrix Rows and Columns;
- Number of Frames;
- Image Type / image flavor;
- Pixel Spacing;
- Dimension Organization Type;
- shared functional groups;
- per-frame functional groups when required;
- optical path identifier;
- image orientation and total-pixel-matrix origin;
- transfer syntax availability where exposed;
- concatenation attributes, even though concatenation support is deferred.

Large per-frame metadata must not be repeatedly downloaded for every tile. It is
loaded once when opening the image, transformed into a compact frame map, and then
owned by the remote image backend.

### Frame Retrieval

Required transaction:

```text
GET {base}/studies/{study}/series/{series}/instances/{instance}/frames/{frames}
Accept: multipart/related; type="{media-type}"; transfer-syntax={uid}
```

Requirements:

- DICOM frame numbers are one-based;
- support one frame and comma-separated batches;
- parse `multipart/related` boundaries and per-part headers;
- preserve the response order and verify it against the requested frame list;
- reject missing, duplicate, or unexpected parts;
- bound part sizes before allocation;
- handle HTTP errors separately from MIME/DICOM errors;
- support cancellation and deadlines;
- retain compressed frames only as long as required by the cache/decode path.

Initial transfer syntax:

- JPEG Baseline, if present in the fixture.

Later transfer syntaxes:

- JPEG 2000 lossless and irreversible;
- JPEG-LS if encountered;
- native/uncompressed pixel data if needed.

The preferred path is raw frame retrieval, not `/rendered`. Raw frames avoid
server-dependent colour conversion and unnecessary re-encoding. Rendered retrieval
may be retained as a debugging fallback but must not silently replace the diagnostic
pixel path.

Reference:

- [DICOMweb Retrieve transaction](https://dicom.nema.org/medical/dicom/current/output/chtml/part18/sect_10.4.html)
- [DICOMweb Store transaction](https://dicom.nema.org/medical/dicom/current/output/chtml/part18/sect_10.5.html)
- [DICOMweb Search transaction](https://dicom.nema.org/medical/dicom/current/output/chtml/part18/sect_10.6.html)

## Proposed Slidescape Client Structure

Do not add DICOMweb conditionals throughout the Slide Score backend. Add a distinct
backend with a reusable HTTP transport boundary.

Suggested files:

```text
src/core/http_client.h
src/core/http_client.c
src/core/multipart.h
src/core/multipart.c
src/core/dicomweb_client.h
src/core/dicomweb_client.c
src/core/dicomweb_image.h
src/core/dicomweb_image.c
```

The names are provisional and should follow the actual refactor boundaries.

Suggested responsibilities:

### HTTP Client

- arbitrary method, headers, and optional streaming request body;
- strict TLS verification;
- response status and headers;
- streaming body callback or bounded buffer;
- connect, read, and total deadlines;
- cancellation;
- safe redirect policy;
- connection reuse;
- bearer header redaction in logs;
- no credential-bearing URLs;
- future HTTP/2 capability without exposing it to the image backend.

### Multipart Parser

- incremental boundary parsing across arbitrary network-buffer boundaries;
- per-part header parsing;
- configurable maximum header and part sizes;
- binary-safe bodies;
- exact final-boundary handling;
- tests for truncated, malformed, and adversarial messages.

### DICOMweb Client

- service base URL and credential provider;
- QIDO request construction and DICOM JSON parsing;
- metadata retrieval;
- frame retrieval and media negotiation;
- error type that preserves HTTP status and DICOM response details;
- no viewer or renderer dependencies.

### DICOMweb Image Backend

Proposed internal data:

```c
typedef struct dicomweb_frame_ref_t {
	char study_uid[65];
	char series_uid[65];
	char instance_uid[65];
	u32 frame_number;
	bool8 exists;
} dicomweb_frame_ref_t;

typedef struct dicomweb_level_t {
	i64 width;
	i64 height;
	u32 tile_width;
	u32 tile_height;
	u32 width_in_tiles;
	u32 height_in_tiles;
	dicomweb_frame_ref_t* frames;
} dicomweb_level_t;
```

UID strings may ultimately be interned or stored once per instance rather than copied
per tile. The example describes semantics, not final memory layout.

Responsibilities:

- identify `VOLUME`, `LABEL`, `OVERVIEW`, and `THUMBNAIL` instances;
- sort volume levels by physical resolution and dimensions;
- construct a frame map;
- calculate `TILED_FULL` frame numbers without materializing unnecessary per-frame
  structures where possible;
- use explicit per-frame plane positions for sparse data later;
- translate a tile cache request to one or more WADO frame requests;
- pass decoded BGRA pixels into the existing cache completion path.

## Test Data

The test corpus must contain only publishable or explicitly machine-local data.
Follow the existing fixture-manifest rules under `tests/fixtures/`.

Minimum initial corpus:

1. A small public DICOM WSI with JPEG Baseline frames and `TILED_FULL`.
2. A public DICOM WSI with label and overview instances.
3. A fixture with at least two pyramid levels in separate instances.
4. A machine-local MRXS bundle representative of the lab scanner.
5. A public MIRAX fixture for repeatable native MRXS parser/converter tests.

Later corpus:

- JPEG 2000;
- `TILED_SPARSE`;
- concatenations;
- multiple optical paths;
- multiple focal planes;
- fluorescence;
- malformed/truncated DICOM JSON and multipart responses.

Every corpus entry should record:

- source and license;
- checksum;
- expected SOP Class;
- expected study/series/instance count;
- expected level dimensions;
- expected frame counts;
- expected transfer syntaxes;
- whether label/overview/thumbnail images exist;
- whether the data are safe to publish.

The DICOM WG-26 WSI page links sample datasets, and IDC provides real public DICOM
WSI:

- [DICOM WSI resources](https://dicom.nema.org/dicom/dicomwsi/index.html)
- [IDC DICOMweb access](https://learn.canceridc.dev/data/downloading-data/dicomweb-access)

## Cross-Server Test Matrix

The same tests and DICOM input must run against Orthanc and dcm4chee.

| Test | Orthanc | dcm4chee | IDC |
| --- | --- | --- | --- |
| STOW single instance | required | required | N/A |
| STOW multi-instance set | required | required | N/A |
| Duplicate STOW behaviour | record | record | N/A |
| QIDO study by UID | required | required | optional |
| QIDO series and instances | required | required | required |
| Series metadata JSON | required | required | required |
| Instance metadata JSON | required | required | required |
| Retrieve one raw frame | required | required | required |
| Retrieve frame batch | required | required | required |
| Preserve JPEG bitstream/TS | inspect | inspect | inspect |
| Label/overview retrieval | required | required | required |
| Invalid frame response | required | required | optional |
| Large metadata behaviour | measure | measure | measure |
| Concurrent frame retrieval | measure | measure | measure |

For every difference, classify it as:

- permitted variation in the DICOM standard;
- unsupported optional capability;
- server defect or configuration issue;
- client assumption that must be removed;
- unclear, requiring a standards review.

Useful independent client or oracle tools:

- `curl`, for raw request/response capture;
- `dicomweb-client`, which implements QIDO, WADO metadata/frames, and STOW;
- `pydicom`, for fixture inspection;
- Slim, for an independent DICOM WSI viewing check;
- OpenSlide, as a secondary source/output comparison where applicable, never as
  the sole correctness oracle for MRXS geometry.

Reference:

- [dicomweb-client conformance](https://dicomweb-client.readthedocs.io/en/latest/conformance.html)
- [dicomweb-client frame examples](https://dicomweb-client.readthedocs.io/en/latest/usage.html)
- [Slim paper](https://arxiv.org/abs/2205.09122)

## MRXS Ingestion

MRXS is a good first vendor format because Slidescape already has a native reader and
there are independent converters. It is not a simple single-file upload.

An MRXS slide consists of:

- a `.mrxs` entry file;
- a same-named companion directory;
- `Slidedat.ini`;
- index data;
- one or more packed data files;
- optional label, macro, preview, and other non-hierarchical images.

The entire bundle is one ingest unit. Copying only the `.mrxs` file is invalid.

OpenSlide describes MIRAX as a multi-file format with complicated proprietary
metadata and indexes:

- [OpenSlide MIRAX format](https://openslide.org/formats/mirax/)

### Converter Direction

#### Slidescape Native MRXS Converter

This is the intended production direction.

The native backend already:

- parses the MRXS entry file, `Slidedat.ini`, index, and packed data files;
- discovers zoom levels and associated barcode/thumbnail images;
- tracks camera positions and the origin of the reconstructed image;
- distinguishes stored tiles from logical viewer/output tiles;
- reconstructs both aligned and fractional camera overlaps;
- returns a regular, non-overlapping logical BGRA tile grid through
  `mrxs_decode_tile_to_bgra()`;
- exposes physical resolution and per-level geometry.

That last distinction is central to conversion. The DICOM writer should consume the
same reconstructed logical image that the viewer displays, not reinterpret raw MRXS
stored-tile coordinates independently.

Recommended component boundary:

```text
MRXS bundle
   |
   v
mrxs_open_from_directory()
   |
   v
native MRXS source adapter
   |  metadata
   |  associated images
   |  logical non-overlapping BGRA tiles
   v
DICOM WSI writer
   |
   v
Part 10 VOLUME / LABEL / OVERVIEW instances
```

The converter should be implemented as reusable non-GUI code plus a command-line
worker entry point. The worker process provides isolation and operational control;
it does not imply using a different image reader.

Suggested boundaries:

```text
src/mrxs/                 existing native source implementation
src/dicom/dicom_write.*   generic DICOM Part 10 writer primitives
src/dicom/dicom_wsi_write.*
                          WSI IOD, functional groups, frames, dimensions
src/core/wsi_convert.*    source-to-DICOM orchestration and validation hooks
```

Exact filenames are provisional. The DICOM writer must not depend on GUI or renderer
state. MRXS runtime tile caches may be reused where safe, but conversion scheduling
and ownership must remain explicit.

Initial native output profile:

- one DICOM Study and one slide-microscopy Series per test slide;
- one `VOLUME` SOP Instance per resolution level;
- `TILED_FULL` organization;
- fixed, configurable output frame size, initially 512 by 512 pixels;
- frame order with columns varying fastest, followed by rows;
- JPEG Baseline transfer syntax for the first implementation;
- RGB/BGRA conversion performed explicitly, with alpha removed only after background
  composition;
- physical pixel spacing and total-matrix origin derived from the validated native
  MRXS reconstruction;
- separate `LABEL` and `OVERVIEW` instances when suitable source associated images
  exist;
- no concatenations in the first small-fixture implementation;
- no focal-plane or optical-path dimension beyond one brightfield path;
- deterministic instance ordering and persisted UIDs.

This output profile must be checked against the current DICOM WSI IOD while it is
implemented. In particular, Image Type values, photometric interpretation, specimen
modules, dimension organization, shared functional groups, ICC profile placement,
and lossy-compression attributes must be derived from the standard and validated
independently. They must not be copied blindly from another converter.

The no-concatenation restriction is acceptable only for the initial controlled
fixtures. Before realistic large MRXS slides are accepted, the writer and upload
pipeline must either support DICOM concatenations or demonstrate that the chosen
archive/client combination safely handles the generated large instances.

Costs and risks:

- DICOM WSI writing is a substantial new feature;
- specimen, optical path, dimension, transfer syntax, and functional-group encoding
  must be standards-correct;
- output tiles will generally need encoding after native overlap reconstruction;
- compressed-tile pass-through is not generally valid because MRXS stored camera
  fragments and overlap geometry do not necessarily match the DICOM total pixel
  matrix;
- colour profile extraction and preservation need specific implementation work;
- the converter output still requires validation by independent DICOM tools and
  archives.

#### wsidicomizer

Role: external comparison implementation and source of DICOM WSI design examples.

- open-source Python library;
- converts proprietary WSI to DICOM WSI;
- MRXS support uses OpenSlide;
- project documentation classifies OpenSlide MRXS conversion as lossy;
- metadata can be supplied or overridden using JSON.

Its output is useful for comparing DICOM object organization, required metadata, and
archive behaviour. It is not the intended production MRXS pixel/geometry path.

Reference:

- [wsidicomizer package and usage](https://pypi.org/project/wsidicomizer/)
- [wsidicomizer source](https://github.com/imi-bigpicture/wsidicomizer)

#### Orthanc WSI Dicomizer

Role: second external comparison implementation.

- designed specifically to create DICOM WSI;
- can use OpenSlide for proprietary formats;
- integrates naturally with the Orthanc test environment;
- provides independently generated DICOM objects for structural comparison.

Reference:

- [Orthanc WSI framework](https://www.orthanc-server.com/static.php?page=wsi)

Neither external converter is a correctness oracle for Slidescape's MRXS output.
Where it differs from them, decide using the source format, decoded physical regions,
the DICOM standard, and visual/quantitative validation.

### MRXS Upload Manifest

A future ingest API should represent the bundle explicitly:

```json
{
  "format": "mrxs",
  "source_name": "slide.mrxs",
  "files": [
    {
      "relative_path": "slide.mrxs",
      "size": 1234,
      "sha256": "..."
    },
    {
      "relative_path": "slide/Slidedat.ini",
      "size": 5678,
      "sha256": "..."
    }
  ],
  "clinical_context": {
    "accession_number": "...",
    "specimen_identifier": "...",
    "container_identifier": "...",
    "issuer": "..."
  }
}
```

Rules:

- paths are relative and normalized;
- absolute paths, `..`, symlinks, and duplicate normalized paths are rejected;
- every file has a cryptographic checksum;
- declared sizes are enforced;
- archive expansion has file-count and total-size limits;
- the bundle remains quarantined until all files and checksums are present;
- the original manifest is immutable;
- corrections create a new ingest attempt linked to the previous attempt.

### Conversion Metadata

Image files alone may not contain enough reliable clinical metadata. The converter
must receive governed context from the LIS/order system or a test manifest.

At minimum, plan for:

- Patient ID and assigning authority;
- Study Instance UID or inputs for generating it;
- accession number;
- specimen identifier and issuer;
- container/slide identifier;
- stain/preparation information;
- acquisition date/time;
- manufacturer, scanner model, and software version;
- physical pixel spacing;
- objective power where reliable;
- optical path;
- barcode;
- source filename and checksum in provenance, not necessarily in patient-visible
  attributes.

For the first non-clinical test corpus, use synthetic/pseudonymous patient and
specimen metadata. Do not derive a patient identity from a filename.

### UID Policy

UIDs must be stable across retries of the same accepted ingest, but a materially
different conversion must not silently overwrite an existing SOP Instance.

Required policy:

- obtain an organisation-controlled DICOM UID root;
- generate Study/Series/SOP UIDs in one component;
- persist the assigned UIDs before conversion starts;
- retries reuse assigned UIDs only when the conversion inputs and converter profile
  are identical;
- changed source checksum, converter version, metadata, tiling, or transfer syntax
  creates a new conversion identity;
- link replacements through application provenance and the appropriate DICOM object
  management policy;
- never derive UIDs from PHI.

Hash-derived UID components are possible, but the canonical serialization and
collision policy must be specified before use.

## Ingest State Machine

Suggested states:

```text
CREATED
  -> UPLOADING
  -> UPLOADED
  -> VALIDATING_SOURCE
  -> READY_FOR_CONVERSION
  -> CONVERTING
  -> VALIDATING_DICOM
  -> STORING
  -> VERIFYING_ARCHIVE
  -> PUBLISHED
```

Failure states:

```text
QUARANTINED
FAILED_RETRYABLE
FAILED_FINAL
CANCELLED
```

Already-converted DICOM enters at `VALIDATING_DICOM` and skips source conversion.

Publication means:

- all required instances were accepted by STOW;
- QIDO finds the expected study, series, and instances;
- WADO metadata matches the submitted instances;
- selected frames can be retrieved and decoded;
- source-to-DICOM validation passed where a source WSI exists;
- the ingest record and audit event are committed.

No partially stored slide should appear in a clinical worklist. Archives may expose
instances immediately after STOW, so the application-level worklist must rely on the
ingest publication state rather than archive presence alone.

## Conversion Validation

Validation has four layers.

### Structural DICOM Validation

- Part 10 parsing succeeds;
- SOP Class is VL Whole Slide Microscopy Image Storage;
- required attributes and functional groups are present;
- UIDs are syntactically valid and internally consistent;
- Number of Frames matches frame data;
- frame and total-matrix dimensions are coherent;
- transfer syntax matches encapsulated pixel data;
- label, overview, and volume image types are coherent;
- DICOM validation-tool output is retained.

### Geometry Validation

Compare source and converted output:

- level count;
- level dimensions;
- downsample factors;
- tile/frame dimensions;
- total pixel matrix;
- microns per pixel;
- physical width and height;
- image bounds/origin;
- orientation;
- overlap/stitching behaviour;
- background fill;
- label and macro orientation.

Sample points must include:

- all four tissue bounds;
- image centre;
- seams between MRXS camera regions;
- empty/tissue boundaries;
- lowest and highest pyramid levels.

### Pixel And Colour Validation

The native converter reconstructs a regular output tile from MRXS camera fragments
and will generally encode that output as a DICOM frame. Bitwise identity with packed
MRXS source fragments is therefore not generally expected, even though the native
reader avoids OpenSlide's geometry limitations.

Required comparisons:

- decode source and DICOM into a common colour representation;
- compare the same physical regions, not merely matching tile indices;
- calculate per-channel error plus an image similarity metric;
- inspect ICC profile preservation/application;
- retain before/after crops for failures;
- test label and overview images separately;
- establish acceptance thresholds with pathology and imaging experts before
  clinical claims.

No numerical threshold in this document is a clinical acceptance threshold.

### Archive Round-Trip Validation

- STOW all instances;
- retrieve metadata;
- retrieve selected raw frames;
- decode returned frames;
- compare returned compressed bytes with submitted frame bytes when the server
  promises the same transfer syntax;
- otherwise compare decoded pixels and record transcoding;
- repeat on both Orthanc and dcm4chee.

## Security Requirements

### Local Development

- bind archives to loopback where practical;
- use synthetic/public fixtures only;
- do not commit credentials or private slides;
- keep server storage in ignored local directories/volumes;
- record exact container image versions;
- do not make disabling TLS verification part of reusable client code.

### Before Shared-Lab Testing

- strict TLS certificate and hostname verification;
- OIDC/OAuth authentication at an API gateway or supported archive boundary;
- short-lived, audience-restricted access tokens;
- no static token files;
- redact authorization, cookies, patient data, and signed URLs from logs;
- per-study/series authorization on both QIDO and WADO;
- upload size, count, rate, and concurrency limits;
- parser/converter workers isolated with CPU, memory, disk, and time limits;
- malware and malformed-container handling;
- encrypted storage and managed keys;
- auditable query, retrieve, store, export, and administrative operations;
- backup and restore tests;
- separate archive administration endpoints from clinical DICOMweb endpoints.

Slidescape's current `remote.c` must not be used as the production security boundary
until its TLS and logging behaviour has been corrected.

## Performance Measurements

Do not set product latency targets from localhost results. The first phase establishes
a repeatable baseline.

Measure:

- STOW throughput and archive indexing delay;
- time from completed STOW to successful QIDO;
- series metadata response size and latency;
- time to first visible tile;
- raw single-frame and batched-frame latency;
- decoded tile throughput;
- concurrent pan/zoom behaviour;
- stale request cancellation;
- archive CPU, memory, and disk reads;
- effect of frame batch size;
- effect of connection reuse;
- transfer syntax preservation versus server transcoding.

Record p50, p95, and maximum for a fixed test scenario. Use cold-cache and warm-cache
runs. A test report must state server version, configuration, fixture checksum,
network conditions, and client build.

## Milestones And Acceptance Criteria

### M0: Fixture And Server Harness

Deliverables:

- public DICOM WSI fixture entries;
- ignored location for private MRXS fixtures;
- pinned Orthanc test configuration;
- pinned dcm4chee test configuration;
- scripts or documented commands to start, reset, and inspect both archives;
- no credentials or generated archive data committed.

Acceptance:

- both servers start from a clean state;
- health checks pass;
- the same fixture can be stored manually in both;
- QIDO and one frame retrieval work with `curl` or `dicomweb-client`.

### M1: DICOMweb Transport Library

Deliverables:

- strict HTTP/TLS request API;
- response header support;
- incremental multipart parser;
- DICOM JSON parser for the required subset;
- unit tests with captured/synthetic responses;
- credential redaction tests.

Acceptance:

- tests cover split boundaries, binary payloads, truncation, excessive sizes, and
  malformed headers;
- the library retrieves metadata and frames from both servers;
- no bearer token is emitted in logs.

### M2: DICOMweb Image Backend

Deliverables:

- open by Study/Series UID;
- instance discovery and pyramid construction;
- `TILED_FULL` tile-to-frame mapping;
- raw JPEG frame fetch and decode;
- tile cache integration;
- label/overview loading where present;
- viewer UI or command-line input for a DICOMweb URL and identifiers.

Acceptance:

- the same slide opens from Orthanc and dcm4chee;
- geometry matches local DICOM opening;
- panning and zooming do not leak or misplace frames;
- closing an image safely cancels/outlives in-flight requests;
- tests cover one-based DICOM frame numbering.

### M3: Cross-Implementation Conformance

Deliverables:

- automated server matrix;
- captured capability/configuration notes;
- documented differences and client workarounds;
- optional IDC read-only run.

Acceptance:

- required matrix rows pass on both local servers;
- no server-specific URL logic exists outside configuration/adapter boundaries;
- unsupported optional behaviour fails clearly.

### M4: Native MRXS-To-DICOM Converter

Deliverables:

- one public and at least one representative private MRXS fixture;
- a native MRXS source adapter that emits deterministic logical tiles and metadata;
- DICOM Part 10 and DICOM WSI writer primitives;
- native Slidescape MRXS-to-DICOM conversion;
- comparison conversion runs using wsidicomizer and Orthanc Dicomizer;
- metadata mapping inputs;
- geometry, pixel, colour, and round-trip reports;
- license/deployment assessment;
- documented differences between native and comparison outputs.

Acceptance:

- native converter output can be stored and viewed through both archives;
- at least one comparison-converter output is processed through the same archive
  tests;
- native/comparison discrepancies are explained using source geometry and DICOM
  requirements;
- reconstructed camera seams and fractional-overlap regions match the native
  Slidescape viewer;
- clinical metadata gaps are enumerated;
- no source bundle is discarded;
- DICOM validator findings are resolved or explicitly documented.

### M5: Ingest Coordinator

Deliverables:

- ingest manifest;
- resumable upload;
- quarantine;
- state machine and idempotency;
- isolated conversion worker;
- STOW and archive verification;
- publication event;
- audit/provenance record.

Acceptance:

- interrupted uploads and conversions resume or fail safely;
- duplicate submissions do not create duplicate clinical slides;
- incomplete studies never enter the application worklist;
- every published DICOM instance links to source checksums and converter version.

## Risks

### Server Accepts But Cannot Efficiently Retrieve WSI Frames

Mitigation: make raw frame latency and transfer syntax preservation explicit in the
M0/M3 test matrix. Generic SOP storage is not sufficient.

### Converter Produces Valid But Geometrically Wrong DICOM

Mitigation: compare physical regions and camera seams against both Slidescape's
native MRXS viewer path and direct native source reads, not just DICOM validator
output. OpenSlide and external converter outputs are secondary comparisons.

### Per-Frame Metadata Is Too Large

Mitigation: prefer `TILED_FULL`, compact metadata into a frame map once, cache it,
and test realistic slide sizes early.

### Archive Transcodes Frames

Mitigation: request explicit media type/transfer syntax, inspect response headers and
bytes, configure codecs, and record any unavoidable transcoding.

### Patient Or Specimen Mismatch

Mitigation: conversion cannot publish based only on embedded image metadata.
Reconcile barcode, accession, specimen, and assigning authority with the order
source.

### MRXS Bundle Is Incomplete

Mitigation: manifest every companion file and checksum before parsing or conversion.

### Client Becomes Coupled To Orthanc

Mitigation: require dcm4chee tests from the first functional client milestone and
use only standard DICOMweb endpoints in production code.

### Scope Expands Into A PACS/LIS Rewrite

Mitigation: keep the first deliverable to already-converted DICOM WSI viewing. New
workflow services require separate design decisions.

## Open Decisions

These must be resolved by evidence or stakeholder choice:

- Which DICOM WSI fixtures may be committed or automatically downloaded?
- Which MRXS scanner versions and brightfield/fluorescence modes are in scope?
- Is JPEG Baseline sufficient for the first DICOM fixture?
- Which HTTP library or transport implementation should replace/underpin
  `remote.c`?
- Should DICOM JSON parsing be implemented locally or through a small vendored
  library?
- What is the organisation's DICOM UID root and UID allocation policy?
- Which system is authoritative for patient, accession, specimen, and stain data?
- What colour-management and visual-fidelity criteria are required for diagnostic
  use?
- Is the production archive expected to be built, bought, or operated as an
  existing product?
- What retention policy applies to original MRXS bundles after validated DICOM
  conversion?

## Handoff Instructions For Future Agents

Before changing code:

1. Read this document and `doc/tile_streamer_handoff.md`.
2. Inspect `git status` and preserve unrelated user files.
3. Confirm the current milestone; do not begin MRXS conversion work while M0-M3
   remain unproven unless explicitly instructed.
4. Check the current DICOM standard and the conformance statement/version of each
   test server. DICOMweb implementations evolve.
5. Use public fixture manifests; never make private WSI required for public tests.
6. Do not edit vendored dependencies unless the task explicitly requires it.
7. Do not reuse `src/server.c` as a production archive.
8. Do not weaken TLS or log credentials to make a test pass.
9. Test every protocol change against both Orthanc and dcm4chee where feasible.
10. Report exactly which servers, versions, transfer syntaxes, fixtures, and
    platforms were tested.

When adding the DICOMweb backend:

- preserve the existing tile cache ownership model;
- keep transport, DICOMweb protocol, frame mapping, and viewer code separate;
- make remote work cancellable and generation-aware;
- avoid copying large metadata and compressed frame buffers unnecessarily;
- preserve 64-bit sizes and offsets;
- bound every untrusted length before allocation;
- add malformed-response tests alongside happy-path tests.

When adding MRXS ingestion:

- treat the MRXS slide as a bundle, not a file;
- retain and checksum the original;
- use the native `src/mrxs/` backend as the production source reader;
- make the DICOM writer consume the reconstructed logical tile grid rather than raw
  stored MRXS tile positions;
- run conversion outside the request process;
- never publish before source, DICOM, and archive round-trip validation;
- record converter name, version, arguments, metadata input, and output checksums;
- validate native MPP, objective power, bounds, camera origin, and overlap handling
  against representative scanner files and known geometry;
- use OpenSlide/wsidicomizer/Orthanc outputs only as independent comparisons, not as
  authoritative MRXS interpretations.

## Primary References

- [DICOM Whole Slide Imaging](https://dicom.nema.org/dicom/dicomwsi/index.html)
- [DICOM PS3.18 Web Services](https://dicom.nema.org/medical/dicom/current/output/chtml/part18/PS3.18.html)
- [IHE Digital Pathology Workflow - Image Acquisition](https://www.ihe.net/uploadedFiles/Documents/PaLM/IHE_PaLM_Suppl_DPIA_Rev1-4_TI_2026-03-06.pdf)
- [OpenSlide MIRAX format](https://openslide.org/formats/mirax/)
- [wsidicomizer](https://pypi.org/project/wsidicomizer/)
- [Orthanc DICOMweb plugin](https://orthanc.uclouvain.be/book/plugins/dicomweb.html)
- [Orthanc WSI framework](https://www.orthanc-server.com/static.php?page=wsi)
- [dcm4chee Archive 5](https://github.com/dcm4che/dcm4chee-arc-light)
- [dcm4che DICOMweb examples](https://dcm4che.github.io/dicomweb/)
- [IDC DICOMweb access](https://learn.canceridc.dev/data/downloading-data/dicomweb-access)
