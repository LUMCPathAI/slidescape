#!/bin/sh

set -eu

base_url="${ORTHANC_URL:-http://127.0.0.1:8042}"
username="${ORTHANC_USERNAME:-slidescape}"
password="${ORTHANC_PASSWORD:-slidescape-local}"
probe="${DICOMWEB_PROBE:-../../../dicomweb_probe}"

if [ ! -x "${probe}" ]; then
	echo "Build dicomweb_probe first, or set DICOMWEB_PROBE." >&2
	exit 1
fi

request() {
	curl --fail --silent --show-error --user "${username}:${password}" "$@"
}

instances="$(request -H "Accept: application/dicom+json" "${base_url}/dicom-web/instances")"
study_uid="$(printf "%s" "${instances}" | jq -er '.[0]["0020000D"].Value[0]')"
series_uid="$(printf "%s" "${instances}" | jq -er '.[0]["0020000E"].Value[0]')"
instance_uid="$(printf "%s" "${instances}" | jq -er '.[0]["00080018"].Value[0]')"
instance_path="studies/${study_uid}/series/${series_uid}/instances/${instance_uid}"

study_search="$(request -H "Accept: application/dicom+json" \
	"${base_url}/dicom-web/studies?0020000D=${study_uid}&limit=1&offset=0")"
printf "%s" "${study_search}" | jq -e --arg uid "${study_uid}" \
	'.[0]["0020000D"].Value[0] == $uid' >/dev/null

metadata_file="$(mktemp "${TMPDIR:-/tmp}/slidescape-dicomweb-metadata.XXXXXX")"
frame_file="$(mktemp "${TMPDIR:-/tmp}/slidescape-dicomweb-frame.XXXXXX")"
headers_file="$(mktemp "${TMPDIR:-/tmp}/slidescape-dicomweb-headers.XXXXXX")"
cleanup() {
	rm -f "${metadata_file}" "${frame_file}" "${headers_file}"
}
trap cleanup EXIT HUP INT TERM

request -H "Accept: application/dicom+json" \
	-o "${metadata_file}" \
	"${base_url}/dicom-web/${instance_path}/metadata"
"${probe}" metadata "${metadata_file}"

request -H "Accept: multipart/related; type=\"image/jpeg\"; transfer-syntax=1.2.840.10008.1.2.4.50" \
	-D "${headers_file}" \
	-o "${frame_file}" \
	"${base_url}/dicom-web/${instance_path}/frames/1"
content_type="$(sed -n 's/^[Cc]ontent-[Tt]ype:[[:space:]]*//p' "${headers_file}" | tr -d '\r' | tail -1)"
"${probe}" frame "${frame_file}" "${content_type}"

echo "Slidescape DICOMweb protocol smoke test passed."
