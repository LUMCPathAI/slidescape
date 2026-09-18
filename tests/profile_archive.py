#!/usr/bin/env python3
"""Profile an isolated archive process; no cache purge and no private fixture dependency.

Example: python3 tests/profile_archive.py --binary ./slidescape_archive \
  --repository data/archive_repo --probe ./dicomweb_probe SERIES_URL [SERIES_URL ...]
Each series starts a fresh archive process (application-cold, filesystem cache uncontrolled).
Temporary response bodies and logs are deleted on exit. JSON timings go to stdout.
"""
import argparse
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import socket
import statistics
import subprocess
import tempfile
import time
from urllib.parse import urlsplit

COMPACT = 'application/vnd.slidescape.wsi-metadata.v1+json'
FRAMES = ', '.join('multipart/related; type="%s"; transfer-syntax=%s' % pair for pair in [
    ('image/jpeg', '1.2.840.10008.1.2.4.50'),
    ('image/jp2', '1.2.840.10008.1.2.4.90'),
    ('image/jp2', '1.2.840.10008.1.2.4.91')])


def request(port, path, accept):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=60)
    start = time.perf_counter()
    try:
        connection.request('GET', path, headers={'Accept': accept})
        response = connection.getresponse()
        first = time.perf_counter()
        body = response.read()
        elapsed = (time.perf_counter() - start) * 1000
        return response.status, response.getheader('Content-Type'), body, elapsed, (first - start) * 1000
    finally:
        connection.close()


def distribution(values):
    ordered = sorted(values)
    return dict(mean_ms=statistics.mean(values), median_ms=statistics.median(values),
                p95_ms=ordered[min(len(ordered) - 1, int(len(ordered) * .95))])


def profile(args, path, directory):
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    log_path = directory / 'server.log'
    with log_path.open('w') as log:
        environment = os.environ.copy()
        if args.server_timings:
            environment['SLIDESCAPE_ARCHIVE_PROFILE'] = '1'
        server = subprocess.Popen([args.binary, 'serve', args.repository, '--port', str(port)],
                                  stdout=log, stderr=log, env=environment)
        try:
            deadline = time.monotonic() + 15
            while 'listening on' not in log_path.read_text():
                if server.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError(log_path.read_text())
                time.sleep(.02)
            status, _, body, qido_ms, _ = request(port, path + '/instances', 'application/dicom+json')
            if status != 200:
                raise RuntimeError(f'QIDO returned HTTP {status}')
            result = dict(qido_ms=qido_ms, instances=[])
            for index, ref in enumerate(json.loads(body)):
                image_type = ref.get('00080008', {}).get('Value', [])
                if len(image_type) > 2 and image_type[2] != 'VOLUME':
                    continue
                instance = path + '/instances/' + ref['00080018']['Value'][0]
                status, content_type, body, elapsed, first = request(port, instance + '/metadata', COMPACT)
                if status != 200 or content_type != COMPACT:
                    raise RuntimeError(f'Compact metadata unavailable: HTTP {status}, {content_type}')
                compact = json.loads(body)
                item = dict(index=index, metadata_bytes=len(body), cold_metadata_ms=elapsed,
                            cold_metadata_ttfb_ms=first, frame_count=compact['frameCount'])
                compact_path = directory / 'compact.json'
                compact_path.write_bytes(body)
                if args.probe:
                    status, _, standard, _, _ = request(port, instance + '/metadata', 'application/dicom+json')
                    if status != 200:
                        raise RuntimeError(f'Standard metadata returned HTTP {status}')
                    standard_path = directory / 'standard.json'
                    standard_path.write_bytes(standard)
                    compared = subprocess.run([args.probe, 'compare', str(standard_path), str(compact_path)],
                                              capture_output=True, text=True, check=True)
                    item['comparison'] = compared.stdout.strip()
                count = compact['frameCount']
                selected = sorted(set([1, max(1, count // 2), count]))
                item['frames'] = []
                for number in selected:
                    uri = instance + '/frames/' + str(number)
                    status, content_type, frame, cold, _ = request(port, uri, FRAMES)
                    if status != 200:
                        raise RuntimeError(f'Frame returned HTTP {status}')
                    samples = [request(port, uri, FRAMES) for _ in range(args.requests)]
                    if any(sample[0] != 200 for sample in samples):
                        raise RuntimeError('Frame sample failed')
                    frame_result = dict(number=number, bytes=len(frame), first_ms=cold,
                                        **distribution([sample[3] for sample in samples]))
                    if args.probe:
                        frame_path = directory / 'frame.multipart'
                        frame_path.write_bytes(frame)
                        decoded = subprocess.run([args.probe, 'profile', str(standard_path),
                                                  str(frame_path), content_type, '30'],
                                                 capture_output=True, text=True, check=True)
                        frame_result['offline_decode'] = decoded.stdout.strip()
                    item['frames'].append(frame_result)
                uri = instance + '/frames/' + ','.join(str(n) for n in range(1, min(count, 4) + 1))
                with concurrent.futures.ThreadPoolExecutor(max_workers=args.clients) as pool:
                    samples = list(pool.map(lambda _: request(port, uri, FRAMES), range(args.requests * args.clients)))
                item['concurrent_batches'] = dict(clients=args.clients, requests=len(samples),
                    failures=sum(sample[0] != 200 for sample in samples),
                    **distribution([sample[3] for sample in samples]))
                result['instances'].append(item)
            if args.server_timings:
                result['server_timings'] = [line for line in log_path.read_text().splitlines()
                                            if line.startswith('archive timing:')]
            return result
        finally:
            server.terminate()
            try:
                server.wait(5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=lambda p: str(Path(p).resolve()))
    parser.add_argument('--repository', required=True, type=lambda p: str(Path(p).resolve()))
    parser.add_argument('--probe', type=lambda p: str(Path(p).resolve()))
    parser.add_argument('--requests', type=int, default=20)
    parser.add_argument('--clients', type=int, default=8)
    parser.add_argument('--server-timings', action='store_true')
    parser.add_argument('series', nargs='+')
    args = parser.parse_args()
    if not 1 <= args.requests <= 10000 or not 1 <= args.clients <= 64:
        parser.error('requests must be 1..10000 and clients 1..64')
    results = []
    for url in args.series:
        path = urlsplit(url).path.rstrip('/')
        if not path.startswith('/dicom-web/studies/') or '/series/' not in path:
            parser.error('Expected a DICOMweb series URL or absolute path')
        with tempfile.TemporaryDirectory(prefix='slidescape-profile-') as directory:
            results.append(profile(args, path, Path(directory)))
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
