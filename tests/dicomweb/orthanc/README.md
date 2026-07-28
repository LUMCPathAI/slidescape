# Orthanc DICOMweb Development Harness

This directory runs a pinned Orthanc reference server for Slidescape DICOMweb
development.

It is deliberately a local test system:

- HTTP is bound only to `127.0.0.1:8042`.
- Credentials are fixed to `slidescape` / `slidescape-local`.
- TLS, OIDC, audit export, backups, and production authorization are absent.
- Archive data live in the disposable Docker volume
  `slidescape-orthanc_orthanc-db`.

Do not expose this configuration to another host or use it for clinical data.

## Start

From this directory:

```sh
docker compose up -d
docker compose ps
```

The Orthanc Explorer is then available at:

```text
http://127.0.0.1:8042/
```

The DICOMweb root is:

```text
http://127.0.0.1:8042/dicom-web/
```

Run the availability and QIDO check:

```sh
./smoke-test.sh
```

Run STOW-RS, QIDO-RS, metadata, and raw frame retrieval against a DICOM
Part 10 WSI instance:

```sh
./smoke-test.sh "/absolute/path/to/volume-instance.dcm"
```

The smoke test requires `curl` and `jq`. It uploads only the file passed on the
command line. Do not pass private data unless local use of that data has been
approved.

Frame retrieval explicitly negotiates JPEG Baseline, matching the fixture and the
dcm4chee test. The client smoke test checks both single-frame retrieval and an ordered
three-frame response. The smoke test also verifies exact Study Instance UID server-side
QIDO matching with `limit` and `offset`.

After building `dicomweb_probe`, run the Slidescape protocol parser against live
Orthanc metadata and frame responses:

```sh
cmake --build ../../../cmake-build-debug --target dicomweb_probe
./client-smoke-test.sh
```

This probe validates Slidescape's DICOM JSON and multipart parsers. Its HTTP
requests use `curl`, independently of the native viewer transport.

## Open In Slidescape

Upload all desired pyramid instances, then choose **File → Open URI** and enter
the series URL:

```text
http://127.0.0.1:8042/dicom-web/studies/{StudyInstanceUID}/series/{SeriesInstanceUID}
```

Enter the local development username and password shown above under **Options**,
or supply them when Slidescape prompts after the first authentication failure.
Slidescape allows unencrypted DICOMweb only for a loopback host; non-local
archives must use verified HTTPS. After a known series opens successfully, the
**Open URI** dialog closes; the archive browser is not opened for a direct
series URL.

To browse instead of opening a known series, enter only the archive root:

```text
http://127.0.0.1:8042/dicom-web
```

Then select **Connect**. The browser queries studies, series,
and logical slide series asynchronously through QIDO-RS. Double-clicking a slide
opens all supported WSI pyramid instances in that series. Raw SOP Instance UIDs
are kept under the selected slide's **Technical details** panel. Other slide
series in the same study are also exposed through **View → DICOM slides**.

For command-line development, credentials can be supplied without putting them
in the URL:

```sh
SLIDESCAPE_DICOMWEB_USERNAME=slidescape \
SLIDESCAPE_DICOMWEB_PASSWORD=slidescape-local \
./Slidescape.app/Contents/MacOS/Slidescape \
  "http://127.0.0.1:8042/dicom-web/studies/{StudyInstanceUID}/series/{SeriesInstanceUID}"
```

The current viewer slice supports `TILED_FULL` brightfield VOLUME instances with
JPEG Baseline or JPEG 2000 frames. It discovers all instances in the series, builds
the pyramid from their Total Pixel Matrix dimensions, maps frames using their
per-frame matrix positions, and retrieves native frames on demand. Tile workers reuse
HTTP/TLS connections and combine up to four consecutive, non-stale requests for the
same instance into one multi-frame retrieval. A missing or invalid part is retried as
an individual frame request.

Use a moderate-size DICOM WSI for interactive frame testing. Orthanc 1.12.11 with
the DICOMweb 1.23 plugin can consume more than the 8 GB Docker VM while retrieving
even one native frame from the local 487 MB / 38,478-frame UMCU instance used by
Slidescape's private parser tests. Docker then reports `OOMKilled=true` and exit 137.
The same instance's metadata and smaller pyramid levels work, and both single- and
multi-frame retrieval work for the smaller levels. This is an Orthanc DICOMweb
large-instance limitation, not a Slidescape frame-map or multipart failure; do not
use that top-level instance as the interactive Orthanc frame oracle.

The Compose profile keeps `WadoRsLoaderThreadsCount` at Orthanc's sequential default.
The Orthanc documentation recommends values above one only for distributed storage;
parallel local reads needlessly increase peak memory pressure.

The console prints cumulative DICOMweb tile statistics on the first request, first
multi-frame request, every 16 requests, and whenever a fallback occurs. Enter
`dicomweb-stats` in the Slidescape console to print the current counters on demand.

Credentials can alternatively be passed as command-line options:

```sh
./Slidescape.app/Contents/MacOS/Slidescape \
  --dicomweb-username slidescape \
  --dicomweb-password slidescape-local \
  "http://127.0.0.1:8042/dicom-web/studies/{StudyInstanceUID}/series/{SeriesInstanceUID}"
```

Environment variables are preferable for passwords because command-line
arguments can be visible in shell history and process listings. Explicit
command-line options override the corresponding environment variables.

## Inspect

```sh
docker compose logs orthanc
curl --user slidescape:slidescape-local \
  -H 'Accept: application/dicom+json' \
  http://127.0.0.1:8042/dicom-web/studies
```

## Stop Or Reset

Stop while preserving archive data:

```sh
docker compose down
```

Delete the local archive volume and all stored instances:

```sh
docker compose down --volumes
```

The reset command is destructive to this development archive only.

## Version And Configuration

The Compose file pins `orthancteam/orthanc:26.6.1`, which supports ARM64 and
AMD64. The official DICOMweb and WSI plugins are enabled. Metadata is requested
in `Full` mode and cached so that WSI metadata retrieval exercises the standard
DICOMweb surface rather than Orthanc's native REST API.

For the supported image and configuration variables, see:

- <https://orthanc.uclouvain.be/book/users/docker-orthancteam.html>
- <https://orthanc.uclouvain.be/book/plugins/dicomweb.html>
