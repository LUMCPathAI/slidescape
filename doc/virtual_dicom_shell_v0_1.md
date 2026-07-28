# Virtual DICOM Shell v0.1

Status: binary format v1, single-instance and aggregate Part 10 writers/loaders, and
transactional multi-instance logical-slide publication are implemented. Aggregate
policy v6 promotes canonically equal governed attributes and
container/specimen/preparation metadata into explicit shared sections.

This document fixes the boundary between immutable source bundles, governed metadata,
virtual DICOM representations, frame providers, and publication. Field names in the
examples are illustrative. An implementation may normalize queried values into
database columns, but it must preserve these semantics.

## Purpose and Aggregate

A virtual DICOM shell is the immutable description of one published pathology slide
representation. It defines the DICOM objects exposed through QIDO/WADO without
requiring every object to exist as a materialized Part 10 file.

```text
source bundle
  ├── immutable source objects
  └── adapter/configuration identity
          ↓
governed metadata version
          ↓
slide representation
  ├── optical paths and color policy
  ├── specimen/container projection
  ├── DICOM series and instance shells
  │     ├── metadata-WADO attribute object
  │     ├── frame provider
  │     └── regular geometry or explicit frame map
  └── validation and provenance references
          ↓
atomic publication
```

One representation describes one logical slide. It may project to multiple DICOM
series and instances, including resolution levels, concatenation parts, LABEL,
OVERVIEW, and THUMBNAIL images. Study and series index rows are projections of the
published representation, not independent sources of truth.

The shell contains or references:

- source-bundle identity and provenance;
- governed patient, study, specimen, container, and acquisition metadata;
- stable DICOM Study, Series, Concatenation, and SOP Instance UIDs;
- deterministic metadata-WADO attributes;
- pixel, geometry, frame-order, dimension, and transfer-syntax contracts;
- immutable provider locators and adapter/transcoder versions;
- embedded ICC profiles and compact frame/payload maps, plus bulk pixel sources by
  digest;
- representation version, validation evidence, and publication state.

It does not inline pixel payloads, source files, expanded millions-of-item functional
group sequences, or unrestricted vendor metadata dumps. ICC profiles and compact
frame/payload maps are embedded so one shell load is sufficient for serving metadata
and locating pixels.

Bulk pixel sources are referenced as:

```text
object {
  sha256
  size
  mediaType
  role
}
```

The digest is stable semantic identity. Database IDs, filesystem paths, object-store
keys, and storage tiers are resolved when the shell is loaded and are not encoded into
the shell.

## Binary Format v1

The shell is one little-endian object. Its exact byte length is retained with its
database record, allowing one allocation and one fixed-length read. A 128-byte header
contains the format version, total length, representation fingerprint, section count,
source count, instance count, and the offset of a fixed-size section directory.

Every section starts on an eight-byte boundary. This small amount of padding allows
numeric arrays to be exposed safely as aligned views into the loaded object. Field
encoding is explicit; the file never persists compiler C struct layouts.

The initial section kinds are:

- source-object table;
- shared string table;
- instance directory;
- typed DICOM attribute table per instance;
- optical-path table with inline, deduplicatable ICC bytes;
- container/specimen identity table per instance;
- optional explicit frame-semantic map;
- payload lookup map.

Directory entries carry section kind, version, owning instance, offset, encoded size,
decoded size, and flags. Unknown critical sections reject the shell. Unknown
noncritical sections may be ignored. Encoded and decoded sizes are distinct so
per-section compression can be added later without changing the directory.

The shared-metadata revision now promotes ten explicitly scoped patient, study, and
series attributes plus canonically equal container/specimen/preparation sections.
The remaining instance attribute tables contain only overrides and image-specific
facts. Shared optical-path data remains a possible later optimization. Shared
sections are never inherited merely because an instance happens to be first. Each
instance projects to a complete independent DICOM metadata dataset. Disagreement in
a governed shared field rejects publication until an explicit governed resolution
policy exists.

The format supports multiple instance records from its first version. The aggregate
Part 10 writer combines validated single-instance sections into one SOP-ordered
logical-slide directory, remapping source, string, section, and payload-source
references without changing the outer format.

Future source-bundle packaging may use an uncompressed ZIP64 container to group
related objects and instances into one logical unit. This is retained as a packaging
option rather than made part of shell format v1.

The format uses 64-bit byte sizes and offsets and 32-bit structural counts. It has no
arbitrary encoded-size ceiling, but all arithmetic is checked, the encoded size must
match the containing object exactly, and the first loader admits at most 65,535
instances.

## Identity and Versioning

The following identities are distinct:

- **Object digest:** SHA-256 of exact immutable bytes.
- **Bundle digest:** digest of the canonical ordered bundle descriptor, including
  every object's digest, size, logical name, and role.
- **Governed metadata version:** immutable version of governed metadata and its
  provenance.
- **Representation fingerprint:** digest over bundle digest, governed metadata
  version, adapter semantic version, adapter policy/configuration digest, and shell
  schema version.
- **Representation ID/version:** archive identity used for publication and audit.
- **DICOM UIDs:** clinical object identities exposed to clients.

For ingested Part 10 DICOM, source UIDs are preserved when published attributes and
pixel semantics are preserved. For virtual objects, UIDs are generated
deterministically beneath a configured organizational UID root from the representation
fingerprint and each object's stable role within the slide.

Replaying the same bundle and policy produces the same fingerprint and UIDs. A
semantics-changing adapter, governed metadata change, frame-order change, color-policy
change, or pixel-policy change creates a new representation version. Any change to
the semantics of a DICOM Instance creates a new SOP Instance UID. Study and Series UID
retention across a correction is controlled by governed identity policy, not chosen
opportunistically by an adapter.

## Instance Shell

Each projected DICOM instance has these logical sections:

```text
identity
  Study, Series, SOP Instance, and SOP Class UIDs
  instance number
  concatenation identity and frame offset, when applicable

classification
  modality, image flavor, frame type
  container and specimen references

pixels
  dimensions, samples, bit depths, and pixel representation
  photometric interpretation
  stored and generated transfer-syntax representations
  lossy compression and derivation history

geometry
  total matrix, tile size, pixel spacing
  slide-coordinate origin and orientation
  tiled organization and frame count
  regular-order parameters or explicit frame-map reference

dimensions
  optical-path and focal-plane definitions and assignments

provider
  kind, immutable locator, and adapter/transcoder semantic version
  promised frame representations

metadata
  canonical DICOM attribute object used by metadata WADO
```

Metadata WADO is projected from the shell's compact typed DICOM attributes and
geometry/dimension sections. DICOM JSON is an HTTP serialization, not a stored shell
artifact. A request handler must not reopen a vendor source to rediscover metadata.
The current projection emits optical paths, specimen descriptions, and typed nested
specimen-preparation content as DICOM sequences and embeds ICC profiles as DICOM JSON
`InlineBinary`.

## Geometry and Frame Maps

A regular single-dimension `TILED_FULL` instance stores tile-grid parameters and
implicit DICOM order. Multidimensional `TILED_FULL`, `TILED_SPARSE`, irregular native
layouts, and concatenations embed a compact frame-semantic section whenever a small
parameter set cannot express DICOM order unambiguously.

For each DICOM frame, a map resolves:

- one-based total-matrix row and column;
- physical X/Y/Z offsets when known;
- zero-based optical-path and focal-plane indices;
- provider-specific lookup coordinates, directly or through a provider-owned index.

DICOM-facing frame order is immutable. Provider byte lookup is encoded independently:
the initial forms are fixed base/stride and a flat array of source index, 64-bit
offset, and 32-bit read size. Future forms may use cumulative offsets or other
summaries. Provider lookup data may be reorganized for performance only if the bytes
or deterministic generation rule promised for each frame remain unchanged.

## Optical Paths and Color

Each optical path has a stable identifier within the representation and may contain:

- description;
- illumination type coded concepts;
- illumination wavelength or coded illumination color;
- light-path and image-path filter characteristics;
- objective and condenser characteristics;
- channel description and recommended display color or palette;
- ICC profile object reference and DICOM Color Space value;
- provenance indicating whether values were preserved, governed, inferred, or
  generated.

The Part 10 adapter layout introduced with recipe v3 writes a 96-byte optical-path
record containing the
identifier and description, DICOM Color Space, illumination wavelength and coded
concepts, filter bands, objective values, field-origin masks, and an aligned
offset/size pair for inline ICC bytes. Pre-v3 optical records are no longer accepted.
A separate compact specimen section retains Container Identifier and each Specimen
Identifier, Specimen UID, and Specimen Short Description.

Aggregate publication retains that optical record layout under adapter recipe/policy
`part10-preserve-v6`, adding the ordered multi-source recipe, logical-slide grouping
contract, and a typed specimen-preparation section. Preparation items are stored in
preorder with explicit specimen, step, and parent-item indices. Concept-name code
components and CODE values retain their coding scheme and optional scheme version;
content relationships are retained alongside explicit parent indices. TEXT, NUM
lexical values and measurement units, UIDREF, DATETIME, DATE, TIME, and PNAME values
remain typed rather than being reduced to a stain string. Development repositories
are recreated rather than migrated; automated representation upgrades remain future
work.

The instance pixel contract separately records photometric interpretation, transfer
syntax, and the path assigned to each frame.

The color policy states:

- whether native encoded pixels are returned unchanged;
- which ICC profile describes them;
- whether a generated representation performs a color transform;
- source and target profiles/color spaces;
- rendering intent and transform implementation/version;
- lossy quality, method, and derivation history where applicable.

A color-transformed or newly lossy result is a different promised frame
representation. It must not silently replace native semantics under the same
representation fingerprint.

ICC profiles are untrusted embedded bytes copied from durable generation inputs.
Ingestion checks the declared profile size, the 128-byte minimum header, padding, and
the `acsp` signature; shell loading repeats those checks after bounds validation.
Deeper tag-table and color-space policy validation remains future work. The archive
does not need to execute an ICC transform during ingestion.

## Provider Contract

A provider advertises representations before frame I/O and resolves one immutable
instance shell. Initial provider kinds are:

- `part10`: reads exact frames from one immutable DICOM Part 10 object;
- `mrxs-native`: maps virtual frames to an immutable MRXS source bundle;
- future native formats using the same contract;
- generated or materialized providers derived from a published shell.

Provider locators contain only stable object references and bounded parameters, never
arbitrary host filesystem paths. Adapter and transcoder semantic versions are pinned
in the representation fingerprint.

Native compressed bytes may pass through only when their DICOM photometric,
dimensions, transfer syntax, color, and lossy-history semantics are exact. Otherwise
the provider exposes a governed generated representation.

## Worked Example: Part 10 Pass-through

The existing IDC-derived fixture becomes one source bundle containing its Part 10
objects. Each instance preserves its source identities and uses a `part10` provider:

```json
{
  "representation": {
    "id": "idc-slide-…",
    "version": 1,
    "bundleDigest": "sha256:…",
    "adapter": {"name": "slidescape-dicom", "semanticVersion": "1"},
    "logicalSlide": {
      "opticalPaths": [{
        "identifier": "0",
        "iccProfile": {"sha256": "…", "size": 141992},
        "colorPolicy": {"nativePixelsUnchanged": true}
      }],
      "instances": [{
        "sopInstanceUID": "1.3.6.1.4.1.…",
        "imageFlavor": "VOLUME",
        "geometry": {
          "organization": "TILED_FULL",
          "frameOrder": {"kind": "regular-row-major"}
        },
        "provider": {
          "kind": "part10",
          "object": {"sha256": "…"},
          "storedTransferSyntax": "1.2.840.10008.1.2.4.90"
        },
        "metadataObject": {"sha256": "…"}
      }]
    }
  }
}
```

The UMCU fixture differs in geometry: its `TILED_SPARSE` instances reference a compact
explicit map built from Per-Frame Functional Groups. The Part 10 object remains the
pixel provider.

## Worked Example: MRXS Native

An MRXS bundle contains the `.mrxs` file and every required companion object with
stable logical names and roles:

```json
{
  "bundle": {
    "sourceFormat": "mrxs",
    "digest": "sha256:…",
    "objects": [
      {"logicalName": "slide.mrxs", "role": "index", "sha256": "…"},
      {"logicalName": "slide/Index.dat", "role": "index", "sha256": "…"},
      {"logicalName": "slide/Data0001.dat", "role": "pixel-data", "sha256": "…"}
    ]
  }
}
```

The implemented P480 spike checksums all of these source members during staging and
uses Slidescape's built-in MRXS reader. It produces:

- governed slide/specimen facts and provenance;
- deterministic DICOM UIDs;
- one aggregate shell with one VOLUME instance per native pyramid level;
- normalized regular-grid geometry and compact implicit frame maps;
- optical-path and color descriptions;
- an `mrxs-native` locator containing the bundle digest, logical source names,
  adapter semantic version, and bounded tile-index parameters.

At request time, frame WADO maps the DICOM frame to native MRXS tile coordinates.
Policy `mrxs-jpeg-v2` returns standalone native JPEG tile bytes without decoding.
For a regular-grid slot with no stored MRXS tile, it generates a deterministic
background JPEG using the level's declared fill color. Consequently the virtual
DICOM instance is `TILED_FULL` even though its native storage is sparse: every
published frame position is retrievable in the selected JPEG Baseline
representation.

This is a constrained development proof: the retained inline recipe contains the
absolute source path, and request-time reads do not rehash the multi-gigabyte bundle.
The manifest supplies admission provenance but not durable ownership. Production must
replace that path with immutable logical object locators before this provider is
cloud-safe. A materialized Part 10 export remains a derivative of the shell, never a
competing metadata authority.

## Ingest and Publication State

All source adapters feed one ingestion state machine:

```text
RECEIVING → STORED → VALIDATING → STAGED → PUBLISHED
                              ↘ REJECTED
```

- `RECEIVING`: request or filesystem import is incomplete and invisible.
- `STORED`: all declared objects are durable and digest-verified.
- `VALIDATING`: a pinned adapter and policy produce governed metadata and shell
  candidates.
- `STAGED`: every referenced immutable object and invariant is validated.
- `PUBLISHED`: one database transaction exposes the representation and all QIDO
  projections.
- `REJECTED`: failure/conflict evidence may be retained, but no representation is
  visible.

STOW-RS, filesystem DICOM import, and MRXS bundle import are source front ends, not
separate publication implementations.

Identical bundle/policy replay returns the existing result. Reuse of a SOP Instance
UID with identical bytes is a no-op. Reuse with different bytes is a conflict unless
an explicit governed replacement policy creates a new representation and valid DICOM
identity.

## v0.1 Decisions and Open Policy

Fixed for implementation:

- one shell aggregate represents one logical slide;
- immutable objects are content-addressed;
- source bundle is the common ingestion unit;
- metadata and frame WADO are deterministic projections of the shell;
- large blobs and frame maps are referenced, not embedded;
- publication is one transactional visibility transition;
- SQLite/filesystem and PostgreSQL/object-store profiles share the same information
  model and conformance tests;
- native format adapters do not leak into DICOMweb handlers.

Required configuration or stakeholder policy:

- organizational UID root and Study/Series UID correction policy;
- authoritative sources and override rules for patient, accession, specimen,
  container, and stain data;
- diagnostic color-management and visual-fidelity acceptance criteria;
- allowed lossy generation profiles;
- retention, replacement, deletion, and legal-hold behavior;
- MRXS scanner/software variants and fluorescence modes in the first supported
  adapter profile.

## Next Implementation Consequence

The single-instance writer materializes one parsed Part 10 instance, including core
typed DICOM attributes, optical paths and ICC bytes, regular or explicit frame
semantics, and fixed-stride, offset/size, or provider-native frame-index payload
lookup. The aggregate writer
combines those validated sections into one shared source/string/instance directory
and preserves an independent provider and payload map for every SOP Instance. The
loader validates bounds and compatibility while retaining views into one
caller-owned buffer.

Validated shells are now stored content-addressed in a dedicated fast tier. Canonical
generation recipes and bundle descriptors retain ordinary immutable object identity,
digests, media types, and foreign-key references. The local SQLite profile normally
stores those control-object bytes inline; placement is neither provenance nor part of
their identity. The recipe digest contributes to the representation fingerprint
embedded in the shell. SQLite publishes the representation, QIDO projection,
provider, stored-frame representation, audit publication, and outbox event in one
visibility transaction.

QIDO reconstruction, metadata WADO, and frame-provider lookup now use the published
representation rows and digest-validated shell buffers. A cache miss performs one
bounded shell read and opens the source object for offset-based frame reads; it does
not reparse Part 10 metadata.

Missing or corrupt Part 10 shells can be regenerated from the retained source and
canonical recipe. Recovery validates both retained objects and the recipe
fingerprint, requires the regenerated size and digest to equal the published shell,
and atomically replaces the fast-tier file without changing publication metadata.
The local profile currently performs bounded lazy repair and serializes it within one
process. Production object storage and durable background repair jobs own
cross-process availability, retry, and telemetry; public request handlers are not the
primary reconstruction orchestrator.

SQLite schema v4 publishes one representation and shell per initial Part 10
Study/Series group. Each instance retains its individual DICOMweb identity and source
provider while referencing its persisted directory index in the aggregate shell.
The aggregate v6 recipe records the SOP-ordered source set and grouping policy, and
local repair can deterministically rebuild and digest-check the complete shell.

A source bundle is not automatically one slide. The current Study/Series grouping is
an explicit first Part 10 policy; governed container/specimen/acquisition identity and
DICOM relationships must supersede or extend it, record ambiguous overrides as
provenance, and participate in the representation fingerprint.

The constrained P480 brightfield MRXS format-neutrality test is implemented. Next,
replace its external filesystem locator with owned source-bundle objects, exercise
publication/repair failure modes, and then add STOW. Fluorescence, associated images,
overlap reconstruction, diagnostic color policy, and additional scanner/software
variants remain later pinned adapter profiles.
