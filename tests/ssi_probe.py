"""Credential-safe live probe for 3DHISTECH Simple Slide Interface.

The probe never prints credentials, SSI session IDs, or slide tokens. Run it
from a terminal whose process environment contains the four SLIDESCAPE_SSI_*
configuration variables documented in tests/ssi/README.md.
"""

from __future__ import annotations

import hashlib
import json
import os
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request


API_PREFIX = "/api/simpleslideinterface/v1"


class ProbeFailure(RuntimeError):
    pass


def request(base: str, method: str, path: str, body: object | None = None,
            accept: str = "application/json") -> tuple[int, bytes]:
    data = None
    headers = {"Accept": accept}
    if body is not None:
        data = json.dumps(body, separators=(",", ":")).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(base.rstrip("/") + path, data=data,
                                 headers=headers, method=method)
    ca_bundle = os.environ.get("SLIDESCAPE_SSI_CA_BUNDLE")
    context = ssl.create_default_context(cafile=ca_bundle) if ca_bundle else ssl.create_default_context()
    try:
        with urllib.request.urlopen(req, context=context, timeout=60) as response:
            return response.status, response.read(4 * 1024 * 1024 + 1)
    except urllib.error.HTTPError as error:
        # Deliberately omit error.url because it can contain an SSI handle.
        raise ProbeFailure(f"HTTP {error.code}") from None
    except Exception as error:
        # Exception messages can contain request URLs, so report only the type.
        raise ProbeFailure(type(error).__name__) from None


def parse_json_string(data: bytes, label: str) -> str:
    value = json.loads(data)
    if not isinstance(value, str) or not value:
        raise ProbeFailure(f"invalid {label} response")
    return value


def main() -> int:
    if len(sys.argv) != 2 or len(sys.argv[1]) != 32:
        print("usage: python tests/ssi_probe.py <32-character-slide-guid>", file=sys.stderr)
        return 2
    slide_guid = sys.argv[1]
    required = {
        "api": os.environ.get("SLIDESCAPE_SSI_API_URL"),
        "slide_center": os.environ.get("SLIDESCAPE_SSI_SLIDECENTER_URL"),
        "username": os.environ.get("SLIDESCAPE_SSI_USERNAME"),
        "password": os.environ.get("SLIDESCAPE_SSI_PASSWORD"),
    }
    missing = [name for name, value in required.items() if not value]
    if missing:
        print("missing SSI environment variables: " + ", ".join(missing), file=sys.stderr)
        return 2

    base = required["api"].rstrip("/")
    session_id = None
    slide_token = None
    try:
        _, data = request(base, "POST", API_PREFIX + "/session/create", {
            "slide_center_url": required["slide_center"],
            "username": required["username"],
            "password": required["password"],
        })
        session_id = parse_json_string(data, "session")
        print("SSI session: created")

        open_path = (API_PREFIX + "/slide/open/server/" +
                     urllib.parse.quote(session_id, safe="") + "/" +
                     urllib.parse.quote(slide_guid, safe="") +
                     "?readonly=true&useOriginalBitMode=false")
        _, data = request(base, "POST", open_path)
        slide_token = parse_json_string(data, "slide-open")
        print("SSI slide: opened read-only")

        token = urllib.parse.quote(slide_token, safe="")
        _, data = request(base, "GET", API_PREFIX + f"/slide/{token}/base_properties")
        props = json.loads(data)
        print("SSI properties:",
              f"{props.get('Width')}x{props.get('Height')}",
              f"mpp={props.get('MicronPerPixelWidth')}x{props.get('MicronPerPixelHeight')}",
              f"levels=0..{props.get('MagnificationLevelMax')}")

        x = int(props["TileXMinNative"])
        y = int(props["TileYMinNative"])
        tile_path = (API_PREFIX + f"/slide/{token}/tile?encoding=JPEG_LOSSY"
                     f"&x1={x}&y1={y}&x2={x}&y2={y}&magnification=0"
                     "&channel_triplet_index=0&output_quality=90")
        _, tile = request(base, "GET", tile_path, accept="application/octet-stream")
        if len(tile) > 4 * 1024 * 1024 or not tile.startswith(b"\xff\xd8"):
            raise ProbeFailure("tile response is not a bounded JPEG")
        print("SSI tile: JPEG", f"bytes={len(tile)}",
              "sha256=" + hashlib.sha256(tile).hexdigest())

        request(base, "POST", API_PREFIX + "/session/" +
                urllib.parse.quote(session_id, safe="") + "/ping")
        request(base, "POST", API_PREFIX + "/slide/" +
                urllib.parse.quote(slide_token, safe="") + "/ping")
        print("SSI keepalive: session and slide pinged")
        return 0
    except ProbeFailure as error:
        print("SSI probe failed:", error, file=sys.stderr)
        return 1
    finally:
        if slide_token:
            try:
                request(base, "DELETE", API_PREFIX + "/slide/" +
                        urllib.parse.quote(slide_token, safe="") + "/close")
                print("SSI slide: closed")
            except ProbeFailure:
                print("SSI slide cleanup: failed", file=sys.stderr)
        if session_id:
            try:
                request(base, "DELETE", API_PREFIX + "/session/" +
                        urllib.parse.quote(session_id, safe="") + "/close")
                print("SSI session: closed")
            except ProbeFailure:
                print("SSI session cleanup: failed", file=sys.stderr)


if __name__ == "__main__":
    raise SystemExit(main())

