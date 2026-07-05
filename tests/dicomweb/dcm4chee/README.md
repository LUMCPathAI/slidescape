# dcm4chee DICOMweb Development Harness

This directory runs a pinned dcm4chee Archive 5 reference server for independent
Slidescape DICOMweb interoperability testing.

It is deliberately a disposable local test system:

- HTTP is bound only to `127.0.0.1:18080`.
- REST services are not authenticated.
- PostgreSQL and LDAP credentials are fixed inside the private Compose network.
- TLS, OIDC, audit export, backups, and production authorization are absent.
- Archive state lives in Docker volumes named with the `slidescape-dcm4chee`
  project prefix.

Do not expose this configuration to another host or use it for clinical data.
The images are substantially larger and startup is slower than the Orthanc
development harness.

## Start

From this directory:

```sh
docker compose up -d
docker compose ps
./smoke-test.sh
```

Initial startup may take several minutes. The DICOMweb root is:

```text
http://127.0.0.1:18080/dcm4chee-arc/aets/DCM4CHEE/rs
```

An empty dcm4chee QIDO-RS search returns `204 No Content`, unlike Orthanc's
`200 OK` with an empty JSON array. The smoke test accepts both successful forms,
and Slidescape models either response as an empty result set.

Run STOW-RS, QIDO-RS, metadata, and native frame retrieval against a DICOM
Part 10 WSI instance:

```sh
./smoke-test.sh "/absolute/path/to/volume-instance.dcm"
```

The smoke test requires `curl` and `jq`. It uploads only the file passed on the
command line.

Frame retrieval explicitly negotiates JPEG Baseline. A wildcard
`application/octet-stream` request causes dcm4chee to return an uncompressed
frame without a per-part transfer-syntax parameter, while Orthanc preserves the
stored JPEG. Explicit media negotiation gives the client the same compressed
representation on both archives.

After building `dicomweb_probe`, validate Slidescape's DICOM JSON and multipart
parsers against live dcm4chee responses:

```sh
cmake --build ../../../cmake-build-debug --target dicomweb_probe
./client-smoke-test.sh
```

Set `DICOMWEB_STUDY_UID`, `DICOMWEB_SERIES_UID`, and `DICOMWEB_INSTANCE_UID` to
target a particular archived instance; otherwise the script selects the first
QIDO result.

## Open In Slidescape

Choose **File → Open URI**, enter the DICOMweb root above, and select
**Connect**. No username or password is needed.

A known slide can also be opened directly:

```text
http://127.0.0.1:18080/dcm4chee-arc/aets/DCM4CHEE/rs/studies/{StudyInstanceUID}/series/{SeriesInstanceUID}
```

## Inspect

```sh
docker compose logs -f arc
curl --fail --silent --show-error \
  -H 'Accept: application/dicom+json' \
  'http://127.0.0.1:18080/dcm4chee-arc/aets/DCM4CHEE/rs/studies?limit=1'
```

## Stop Or Reset

Stop while preserving test data:

```sh
docker compose down
```

Delete all dcm4chee test configuration and stored instances:

```sh
docker compose down --volumes
```

The reset command is destructive only to this Compose project's development
volumes.

## Version And Configuration

The harness pins:

- `dcm4che/dcm4chee-arc-psql:5.35.0`
- `dcm4che/slapd-dcm4chee:2.6.13-35.0`
- `dcm4che/postgres-dcm4chee:18.3-35`

The containers use `linux/amd64` explicitly because the dcm4chee image set does
not consistently publish native images for every development host architecture.
