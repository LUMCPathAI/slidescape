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

Frame retrieval requests the stored transfer syntax using
`application/octet-stream; transfer-syntax=*`. This avoids asking the archive to
transcode the tile and verifies the path a Slidescape client should normally use.

After building `dicomweb_probe`, run the Slidescape protocol parser against live
Orthanc metadata and frame responses:

```sh
cmake --build ../../../cmake-build-debug --target dicomweb_probe
./client-smoke-test.sh
```

This probe validates Slidescape's DICOM JSON and multipart parsers. Its HTTP
requests still use `curl`; native Slidescape HTTP/TLS transport integration is
the next implementation step.

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
