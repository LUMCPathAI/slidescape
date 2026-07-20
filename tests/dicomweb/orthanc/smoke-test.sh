#!/bin/sh

set -eu

base_url="${ORTHANC_URL:-http://127.0.0.1:8042}"
username="${ORTHANC_USERNAME:-slidescape}"
password="${ORTHANC_PASSWORD:-slidescape-local}"
dicom_file="${1:-}"

request() {
	curl --fail --silent --show-error \
		--user "${username}:${password}" \
		"$@"
}

echo "Checking Orthanc REST API..."
request "${base_url}/system" | jq -e '.Name and .Version' >/dev/null

echo "Checking empty-or-populated QIDO-RS study search..."
request \
	-H "Accept: application/dicom+json" \
	"${base_url}/dicom-web/studies" | jq -e 'type == "array"' >/dev/null

if [ -z "${dicom_file}" ]; then
	echo "Orthanc and QIDO-RS are ready."
	echo "Pass a DICOM Part 10 file to also test STOW, metadata, and frame retrieval."
	exit 0
fi

if [ ! -f "${dicom_file}" ]; then
	echo "DICOM file does not exist: ${dicom_file}" >&2
	exit 1
fi

boundary="slidescape-stow-$$"
body_file="$(mktemp "${TMPDIR:-/tmp}/slidescape-stow.XXXXXX")"
headers_file="$(mktemp "${TMPDIR:-/tmp}/slidescape-headers.XXXXXX")"
frame_file="$(mktemp "${TMPDIR:-/tmp}/slidescape-frame.XXXXXX")"
stow_file="$(mktemp "${TMPDIR:-/tmp}/slidescape-stow-response.XXXXXX")"
cleanup() {
	rm -f "${body_file}" "${headers_file}" "${frame_file}" "${stow_file}"
}
trap cleanup EXIT HUP INT TERM

{
	printf -- "--%s\r\n" "${boundary}"
	printf "Content-Type: application/dicom\r\n\r\n"
	cat "${dicom_file}"
	printf "\r\n--%s--\r\n" "${boundary}"
} > "${body_file}"

echo "Storing one instance through STOW-RS..."
request \
	-X POST \
	-H "Accept: application/dicom+json" \
	-H "Content-Type: multipart/related; type=\"application/dicom\"; boundary=${boundary}" \
	--data-binary "@${body_file}" \
	-o "${stow_file}" \
	"${base_url}/dicom-web/studies"

instance_uid="$(jq -er '.["00081199"].Value[0]["00081155"].Value[0]' "${stow_file}")"

echo "Discovering stored identifiers through QIDO-RS..."
instances_json="$(request -H "Accept: application/dicom+json" \
	"${base_url}/dicom-web/instances?SOPInstanceUID=${instance_uid}")"
study_uid="$(printf "%s" "${instances_json}" | jq -er '.[0]["0020000D"].Value[0]')"
series_uid="$(printf "%s" "${instances_json}" | jq -er '.[0]["0020000E"].Value[0]')"

echo "Retrieving instance metadata..."
request \
	-H "Accept: application/dicom+json" \
	"${base_url}/dicom-web/studies/${study_uid}/series/${series_uid}/instances/${instance_uid}/metadata" \
	| jq -e 'type == "array" and length == 1' >/dev/null

echo "Retrieving raw frame 1..."
request \
	-D "${headers_file}" \
	-o "${frame_file}" \
	-H "Accept: multipart/related; type=\"image/jpeg\"; transfer-syntax=1.2.840.10008.1.2.4.50" \
	"${base_url}/dicom-web/studies/${study_uid}/series/${series_uid}/instances/${instance_uid}/frames/1"

if ! grep -Eiq '^content-type:[[:space:]]*multipart/related' "${headers_file}"; then
	echo "Frame response was not multipart/related." >&2
	exit 1
fi
if [ ! -s "${frame_file}" ]; then
	echo "Frame response was empty." >&2
	exit 1
fi

echo "Orthanc DICOMweb smoke test passed."
echo "Study:   ${study_uid}"
echo "Series:  ${series_uid}"
echo "Instance:${instance_uid}"
