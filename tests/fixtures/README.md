# Test Fixtures

Fixture tests are driven by `manifest.txt` plus an optional machine-local
`local_manifest.txt`.

Fixture files are not downloaded by the test runner. Download public fixtures
into this directory, or add private fixtures with paths relative to
`local_manifest.txt`.

The tracked public manifest currently includes:

- `testslide.isyntax` from `https://zenodo.org/record/5037046/files/testslide.isyntax`
- the IDC CGCI-HTMCP-DLBCL DICOM WSI series linked from `manifest.txt`. Place its
  four Part 10 instances below `dicom/cgci_htmcp_dlbcl/`; subdirectories may retain
  the IDC collection/patient/study/series hierarchy because the DICOM fixture reader
  discovers instances recursively.

There is intentionally no repository-owned IDC download script. IDC export tools and
object manifests may produce flat, release-specific layouts; fixture provisioning
should preserve an intelligible collection/patient/study/series tree and must record
the exact IDC release plus checksums in CI configuration before the fixture is made a
required CI dependency. The public fixture remains optional when its directory is not
present locally.

Additional public WSI files can be added from sources such as
`https://downloads.openmicroscopy.org/images/` when they are useful for a
specific format-reader regression.

For local private fixtures, copy `local_manifest.example.txt` to
`local_manifest.txt` and edit the paths if needed.

Archive tests create only small disposable repositories. On Unix they prefer
`/dev/shm` when available, otherwise `TMPDIR` and then `/tmp`. Set
`SLIDESCAPE_TEST_TMPDIR` to a writable RAM-disk directory to force all test
repository and temporary-file writes there. Real DICOM fixture coverage does not
copy volume instances into those repositories: the provider integration workflow
selects one suitable source no larger than 16 MiB, while parsing, shell, frame-map,
and decoding checks continue to read every configured fixture directly.
