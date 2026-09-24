#!/usr/bin/env python3
"""Compare fresh and reused HTTP connections to one archive response.

Pass a WADO frame URL for a tile-path benchmark. The standard library is enough;
an SSH local forward can supply the URL's localhost endpoint.
"""

import argparse
import concurrent.futures
import http.client
import statistics
import time
from urllib.parse import urlsplit


def percentile(values, percentage):
    ordered = sorted(values)
    return ordered[int((len(ordered) - 1) * percentage / 100)]


def run_worker(host, port, path, accept, count, reuse):
    timings = []
    opened = 0
    body_size = None
    connection = None
    previous_socket = None
    try:
        for _ in range(count):
            if connection is None:
                connection = http.client.HTTPConnection(host, port, timeout=15)
            started = time.perf_counter()
            connection.request('GET', path, headers={'Accept': accept} if accept else {})
            if connection.sock is not previous_socket:
                opened += 1
                previous_socket = connection.sock
            response = connection.getresponse()
            body = response.read()
            elapsed_ms = (time.perf_counter() - started) * 1000
            if response.status != 200 or len(body) != int(response.getheader('Content-Length', -1)):
                raise RuntimeError(f'HTTP {response.status}, {len(body)} response bytes')
            if body_size is None:
                body_size = len(body)
            elif len(body) != body_size:
                raise RuntimeError('Response size changed during benchmark')
            timings.append(elapsed_ms)
            if not reuse:
                connection.close()
                connection = None
                previous_socket = None
    finally:
        if connection is not None:
            connection.close()
    return timings, opened, body_size


def run_mode(host, port, path, accept, requests, clients, reuse):
    counts = [requests // clients + (i < requests % clients) for i in range(clients)]
    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=clients) as pool:
        results = list(pool.map(lambda count: run_worker(host, port, path, accept, count, reuse), counts))
    wall_seconds = time.perf_counter() - started
    timings = [timing for result in results for timing in result[0]]
    opens = sum(result[1] for result in results)
    sizes = {result[2] for result in results}
    if len(sizes) != 1:
        raise RuntimeError('Response sizes varied across clients')
    label = 'reuse' if reuse else 'fresh'
    print(f'{label}: requests={len(timings)} clients={clients} connections={opens} '
          f'body={sizes.pop()} bytes median={statistics.median(timings):.2f} ms '
          f'p95={percentile(timings, 95):.2f} ms max={max(timings):.2f} ms '
          f'throughput={len(timings) / wall_seconds:.1f} requests/s')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('url', help='Complete http:// URL of one archive response')
    parser.add_argument('--accept', help='Accept header for WADO representation negotiation')
    parser.add_argument('--requests', type=int, default=40)
    parser.add_argument('--clients', type=int, default=4)
    args = parser.parse_args()
    if args.requests < 1 or args.clients < 1 or args.clients > args.requests:
        parser.error('require 1 <= clients <= requests')
    url = urlsplit(args.url)
    if url.scheme != 'http' or not url.hostname or url.username or url.password or url.fragment:
        parser.error('expected a complete http:// URL without credentials or fragment')
    path = url.path or '/'
    if url.query:
        path += '?' + url.query
    host, port = url.hostname, url.port or 80
    run_worker(host, port, path, args.accept, 1, False)
    run_mode(host, port, path, args.accept, args.requests, args.clients, False)
    run_mode(host, port, path, args.accept, args.requests, args.clients, True)


if __name__ == '__main__':
    main()
