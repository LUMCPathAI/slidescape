#!/bin/sh

set -eu

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repository_root="$(CDPATH= cd -- "${script_dir}/../.." && pwd)"
probe="${DICOMWEB_PROBE:-${repository_root}/dicomweb_probe}"

if [ ! -x "${probe}" ]; then
	echo "Build dicomweb_probe first, or set DICOMWEB_PROBE." >&2
	exit 1
fi

echo "Testing Slidescape protocol profile against Orthanc..."
DICOMWEB_PROBE="${probe}" "${script_dir}/orthanc/client-smoke-test.sh"

echo "Testing Slidescape protocol profile against dcm4chee..."
DICOMWEB_PROBE="${probe}" "${script_dir}/dcm4chee/client-smoke-test.sh"

if [ -n "${SLIDESCAPE_EXECUTABLE:-}" ]; then
	echo "Testing Slidescape credential redaction against Orthanc..."
	"${script_dir}/credential-redaction-test.sh"
fi

echo "Slidescape DICOMweb client matrix passed."
