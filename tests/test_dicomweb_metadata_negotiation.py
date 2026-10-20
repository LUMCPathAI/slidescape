#!/usr/bin/env python3
"""Exercise compact-metadata fallback through the real headless viewer, offline."""
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from urllib.parse import urlsplit

COMPACT = 'application/vnd.slidescape.wsi-metadata.v1+json'
STANDARD = 'application/dicom+json'
STUDY, SERIES = '1.2.3', '1.2.3.4'
INSTANCES = ('1.2.3.4.1', '1.2.3.4.2')


def metadata(index):
    def tag(vr, *values):
        return {'vr': vr, 'Value': list(values)}
    size = 4 >> index
    return {
        '00080008': tag('CS', 'DERIVED', 'PRIMARY', 'VOLUME', 'RESAMPLED'),
        '00080016': tag('UI', '1.2.840.10008.5.1.4.1.1.77.1.6'),
        '00080018': tag('UI', INSTANCES[index]),
        '0020000D': tag('UI', STUDY), '0020000E': tag('UI', SERIES),
        '00280002': tag('US', 3), '00280004': tag('CS', 'RGB'),
        '00280008': tag('IS', (size // 2) ** 2),
        '00280010': tag('US', 2), '00280011': tag('US', 2),
        '00480006': tag('UL', size), '00480007': tag('UL', size),
        '00209311': tag('CS', 'TILED_FULL'),
        '52009229': tag('SQ', {'00289110': tag('SQ', {
            '00280030': tag('DS', str(.001 * (1 << index)), str(.001 * (1 << index)))})}),
    }


def exercise(binary, compact_status, standard_status):
    requests = []

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            path = urlsplit(self.path).path
            status, body = 200, []
            if path.endswith('/metadata'):
                uid = path.split('/')[-2]
                accept = self.headers.get('Accept')
                requests.append((uid, accept))
                if accept == COMPACT:
                    status = compact_status
                elif accept == STANDARD:
                    status, body = standard_status, [metadata(INSTANCES.index(uid))]
                else:
                    status = 406
            elif path.endswith('/instances'):
                body = [metadata(i) for i in range(2)]
            elif path.endswith('/series'):
                body = [{'0020000E': {'vr': 'UI', 'Value': [SERIES]}}]
            else:
                status = 404
            payload = json.dumps(body).encode() if status == 200 else b''
            self.send_response(status)
            self.send_header('Content-Type', STANDARD)
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    with ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            url = f'http://127.0.0.1:{server.server_port}/dicomweb/studies/{STUDY}/series/{SERIES}'
            # Keep viewer settings or other incidental files out of the checkout.
            with tempfile.TemporaryDirectory(prefix='slidescape-dicomweb-test-') as directory:
                result = subprocess.run([binary, '--headless', url], cwd=directory,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, timeout=20)
        finally:
            server.shutdown()
            worker.join()
    if compact_status in (406, 500):
        expected = [(INSTANCES[0], COMPACT), (INSTANCES[0], STANDARD), (INSTANCES[1], STANDARD)]
    else:
        expected = [(uid, COMPACT) for uid in INSTANCES]
    assert requests == expected, (requests, expected, result.stdout)
    success = compact_status in (406, 500) and standard_status == 200
    assert (result.returncode == 0) == success, result.stdout
    if success:
        assert result.stdout.count('Opened DICOMweb series with 2 pyramid instances.') == 1, result.stdout
    else:
        status = standard_status if compact_status in (406, 500) else compact_status
        assert result.stdout.count(f'last HTTP status: {status}') == 1, result.stdout


if __name__ == '__main__':
    executable = str(Path(sys.argv[1]).resolve())
    for compact, standard in ((500, 200), (406, 200), (500, 500), (401, 200)):
        exercise(executable, compact, standard)
    print('Metadata negotiation: 500/406 fallback, bounded genuine 500 failure, and no auth retry passed.')
