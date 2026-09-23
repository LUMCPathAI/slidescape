# DICOMweb reference-server matrix

The offline `slidescape_dicomweb_metadata_negotiation` CTest runs the headless
viewer against a temporary loopback server. It checks fallback to standard
DICOM JSON when compact metadata is rejected with HTTP 500 (as observed on IDC)
or 406, that later instances skip the rejected extension, and that real server
and authentication failures remain errors. Build `slidescape` first
(`slidescape_console` on Windows); this test is enabled when Python 3 is found.

Orthanc and dcm4chee are independent black-box reference implementations. They are
not the intended production archive.

After starting both Compose harnesses and populating each with at least one matching
JPEG Baseline DICOM WSI instance, build the parser probe and run:

```sh
cmake --build ../../cmake-build-debug --target dicomweb_probe
./run-client-matrix.sh
```

The matrix verifies exact Study Instance UID server-side QIDO matching, instance
metadata parsing, explicit JPEG Baseline frame negotiation, and multipart parsing on
both servers.

To include the verbose-output credential regression against Orthanc:

```sh
SLIDESCAPE_EXECUTABLE=/absolute/path/to/Slidescape \
  ./run-client-matrix.sh
```

The redaction test deliberately uses an invalid password marker, expects the headless
open to fail, and fails if that marker appears in Slidescape output.

The harnesses currently rely on already-populated local volumes. A redistributable
public DICOM WSI fixture and repeatable provisioning lifecycle are still required
before enabling the matrix in CI.
