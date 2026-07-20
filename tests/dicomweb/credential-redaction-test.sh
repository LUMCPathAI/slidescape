#!/bin/sh

set -eu

base_url="${ORTHANC_URL:-http://127.0.0.1:8042}"
username="${ORTHANC_USERNAME:-slidescape}"
password="${ORTHANC_PASSWORD:-slidescape-local}"
slidescape="${SLIDESCAPE_EXECUTABLE:-}"

if [ -z "${slidescape}" ] || [ ! -x "${slidescape}" ]; then
	echo "Set SLIDESCAPE_EXECUTABLE to a built Slidescape executable." >&2
	exit 1
fi

instances="$(curl --fail --silent --show-error --user "${username}:${password}" \
	-H "Accept: application/dicom+json" "${base_url}/dicom-web/instances")"
study_uid="$(printf "%s" "${instances}" | jq -er '.[0]["0020000D"].Value[0]')"
series_uid="$(printf "%s" "${instances}" | jq -er '.[0]["0020000E"].Value[0]')"
series_url="${base_url}/dicom-web/studies/${study_uid}/series/${series_uid}"

output_file="$(mktemp "${TMPDIR:-/tmp}/slidescape-redaction.XXXXXX")"
cleanup() {
	rm -f "${output_file}"
}
trap cleanup EXIT HUP INT TERM

secret_marker="slidescape-secret-must-not-appear-7f4c9d"
if env SLIDESCAPE_DICOMWEB_USERNAME="redaction-test" \
	SLIDESCAPE_DICOMWEB_PASSWORD="${secret_marker}" \
	"${slidescape}" --verbose --headless "${series_url}" >"${output_file}" 2>&1; then
	echo "Expected invalid credentials to make the headless open fail." >&2
	exit 1
fi

if grep -F "${secret_marker}" "${output_file}" >/dev/null; then
	echo "Slidescape output exposed the credential marker." >&2
	exit 1
fi

echo "Slidescape credential redaction test passed."
