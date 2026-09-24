#!/usr/bin/env python3
"""Public synthetic Part 10 / CLI / HTTP regression tests; standard library only."""
import concurrent.futures
import http.client
import hashlib
import json
import os
import shutil
from pathlib import Path
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

SOP_CLASS = '1.2.840.10008.5.1.4.1.1.77.1.6'
TRANSFER_SYNTAX = '1.2.840.10008.1.2.1'


def element(group, tag, vr, value):
    if isinstance(value, str):
        value = value.encode('ascii')
    if len(value) % 2:
        value += b'\0' if vr in ('UI', 'OB') else b' '
    prefix = struct.pack('<HH', group, tag) + vr.encode('ascii')
    if vr in ('OB', 'OW', 'SQ', 'UN', 'UT'):
        return prefix + b'\0\0' + struct.pack('<I', len(value)) + value
    return prefix + struct.pack('<H', len(value)) + value


def sequence(group, tag, *items):
    return element(group, tag, 'SQ', b''.join(
        struct.pack('<HHI', 0xfffe, 0xe000, len(item)) + item for item in items))


def write_slide(path, number, series=None, *, truncated=False, sparse=False):
    study = '2.25.100'
    series = f'2.25.{series or (200 + number)}'
    sop = f'2.25.{300 + number}'
    e = element
    us = lambda tag, value: e(0x28, tag, 'US', struct.pack('<H', value))
    data = b'\0' * 128 + b'DICM'
    data += e(2, 2, 'UI', SOP_CLASS) + e(2, 3, 'UI', sop)
    data += e(2, 0x10, 'UI', TRANSFER_SYNTAX)
    data += e(8, 8, 'CS', 'DERIVED\\PRIMARY\\VOLUME\\RESAMPLED')
    data += e(8, 0x16, 'UI', SOP_CLASS) + e(8, 0x18, 'UI', sop) + e(8, 0x60, 'CS', 'SM')
    data += e(0x20, 0x0d, 'UI', study) + e(0x20, 0x0e, 'UI', series)
    data += e(0x20, 0x13, 'IS', '1')
    data += e(0x20, 0x9311, 'CS', 'TILED_SPARSE' if sparse else 'TILED_FULL')
    data += us(2, 3) + e(0x28, 4, 'CS', 'RGB') + us(6, 0)
    data += e(0x28, 8, 'IS', '2') + us(0x10, 2) + us(0x11, 2)
    data += us(0x100, 8) + us(0x101, 8) + us(0x102, 7) + us(0x103, 0)
    data += e(0x48, 6, 'UL', struct.pack('<I', 4)) + e(0x48, 7, 'UL', struct.pack('<I', 2))
    data += sequence(0x48, 8, e(0x40, 0x72a, 'DS', '12.5') +
                     e(0x40, 0x73a, 'DS', '7.25') + e(0x40, 0x74a, 'DS', '125'))
    data += e(0x48, 0x102, 'DS', '1\\0\\0\\0\\1\\0')
    data += sequence(0x48, 0x105, e(0x48, 0x106, 'SH', 'A'))
    data += e(0x48, 0x302, 'UL', struct.pack('<I', 1))
    data += e(0x48, 0x303, 'UL', struct.pack('<I', 1))
    data += sequence(0x5200, 0x9229, sequence(0x28, 0x9110, e(0x28, 0x30, 'DS', '0.5\\0.5')))
    if sparse:
        frames = []
        for column, x in [(3, '13.5'), (1, '12.5')]:
            position = e(0x48, 0x21e, 'SL', struct.pack('<i', column))
            position += e(0x48, 0x21f, 'SL', struct.pack('<i', 1))
            position += e(0x40, 0x72a, 'DS', x) + e(0x40, 0x73a, 'DS', '7.25')
            position += e(0x40, 0x74a, 'DS', '125')
            frames.append(sequence(0x48, 0x21a, position) +
                          sequence(0x48, 0x207, e(0x48, 0x106, 'SH', 'A')))
        data += sequence(0x5200, 0x9230, *frames)
    pixels = bytes(range(24)) if not truncated else b'\0\1'
    data += e(0x7fe0, 0x10, 'OB', pixels)
    path.write_bytes(data)
    return f'/dicom-web/studies/{study}/series/{series}/instances/{sop}'


def run(binary, *args, success=True):
    result = subprocess.run([binary, *map(str, args)], capture_output=True, text=True, timeout=30)
    assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
    return result


def request(port, path, accept=None):
    client = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
    try:
        client.request('GET', path, headers={'Accept': accept} if accept else {})
        response = client.getresponse()
        payload = response.read()
        assert int(response.getheader('Content-Length')) == len(payload)
        return response.status, dict(response.getheaders()), payload
    finally:
        client.close()


def main(binary, exercise_deadline=False):
    with tempfile.TemporaryDirectory(prefix='slidescape-archive-integration-') as temp:
        root = Path(temp)
        repository = root / 'repository'
        sources = root / 'sources'
        sources.mkdir()
        a = write_slide(sources / 'a.dcm', 1)
        run(binary, 'init', repository)
        run(binary, 'ingest', repository, '--source', sources / 'a.dcm')
        database = sqlite3.connect(repository / 'archive.sqlite3')
        def scalar(sql):
            return database.execute(sql).fetchone()[0]
        original = database.execute('SELECT fingerprint, bundle_id FROM representation').fetchone()
        b = write_slide(sources / 'b.dcm', 2, sparse=True)
        run(binary, 'ingest', repository, '--source', sources)
        run(binary, 'ingest', repository, '--source', sources)
        assert scalar('SELECT COUNT(*) FROM publication') == 2
        assert database.execute('SELECT fingerprint, bundle_id FROM representation ORDER BY id LIMIT 1').fetchone() == original
        assert scalar("SELECT COUNT(*) FROM bundle WHERE state='PUBLISHED'") == 2
        # An incomplete pixel payload must never become a published bundle.
        bad = root / 'bad.dcm'
        write_slide(bad, 3, truncated=True)
        run(binary, 'ingest', repository, '--source', bad, success=False)
        assert scalar('SELECT COUNT(*) FROM publication') == 2
        assert scalar("SELECT COUNT(*) FROM bundle WHERE state='STAGED'") == 1
        # A new group and a changed existing group must roll back publication together.
        conflict = root / 'conflict'
        conflict.mkdir()
        write_slide(conflict / 'c.dcm', 4)
        write_slide(conflict / 'extra.dcm', 5, series=201)
        run(binary, 'ingest', repository, '--source', conflict, success=False)
        assert scalar('SELECT COUNT(*) FROM publication') == 2
        assert scalar("SELECT COUNT(*) FROM bundle WHERE state='PUBLISHED'") == 2
        # Additional generated representations must not duplicate source index rows.
        database.execute("INSERT INTO frame_representation SELECT instance_id, 'image/jpeg', "
                         "'1.2.840.10008.1.2.4.50', 'GENERATED', source_object_id, 'test', 200 "
                         "FROM frame_representation WHERE source='STORED'")
        database.commit()
        database.close()
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        log = root / 'server.log'
        with log.open('w+') as output:
            server = subprocess.Popen([binary, 'serve', str(repository), '--port', str(port)],
                                      stdout=output, stderr=output)
            try:
                deadline = time.monotonic() + 10
                while 'listening on' not in log.read_text():
                    assert server.poll() is None, log.read_text()
                    assert time.monotonic() < deadline, log.read_text()
                    time.sleep(.02)
                status, _, health = request(port, '/health')
                assert status == 200 and json.loads(health)['repositorySchemaVersion'] == 2
                # Two cold requests share one validated shell, with exact JSON semantics.
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                    results = list(pool.map(lambda _: request(port, a + '/metadata'), range(2)))
                for status, _, body in results:
                    assert status == 200, body
                    metadata = json.loads(body)[0]
                    assert metadata['00080008']['Value'] == ['DERIVED', 'PRIMARY', 'VOLUME', 'RESAMPLED']
                    for tag, value in [('00280006', 0), ('00280102', 7), ('00280103', 0)]:
                        assert metadata[tag]['Value'] == [value]
                    assert metadata['00480102']['Value'] == [1, 0, 0, 0, 1, 0]
                    assert metadata['00480008']['Value'][0]['0040074A']['Value'] == [125]
                health = json.loads(request(port, '/health')[2])
                assert health['shellCache']['misses'] == 1, health
                status, _, body = request(port, b + '/metadata')
                assert status == 200, body
                metadata = json.loads(body)[0]
                assert metadata['00209311']['Value'] == ['TILED_SPARSE']
                frames = metadata['52009230']['Value']
                assert len(frames) == 2
                assert frames[0]['0048021A']['Value'][0]['0048021E']['Value'] == [3]
                assert frames[0]['0048021A']['Value'][0]['0040072A']['Value'] == [13.5]
                assert frames[0]['0048021A']['Value'][0]['0040074A']['Value'] == [125]
                assert frames[0]['00480207']['Value'][0]['00480106']['Value'] == ['A']
                compact_type = 'application/vnd.slidescape.wsi-metadata.v1+json'
                for path, full, runs in [(a, True, []), (b, False, [[1, 1, 1], [0, 2, 1]])]:
                    status, headers, body = request(port, path + '/metadata', compact_type)
                    assert status == 200 and headers['Content-Type'] == compact_type, body
                    compact = json.loads(body)
                    assert compact['version'] == 1 and compact['tiledFull'] == full
                    assert compact['runs'] == runs and compact['frameCount'] == 2
                    assert compact['matrixColumns'] == 4 and compact['mpp'] == [500, 500]
                    assert compact['instanceUID'] == path.rsplit('/', 1)[1]
                    # The extension never changes the standard representation.
                    assert '00080008' in json.loads(request(port, path + '/metadata')[2])[0]
                accept = 'multipart/related; type="application/octet-stream"; transfer-syntax=' + TRANSFER_SYNTAX
                status, headers, body = request(port, a + '/frames/2,1,2', accept)
                assert status == 200, body
                boundary = headers['Content-Type'].split('boundary=')[1].encode()
                parts = body.split(b'--' + boundary)[1:-1]
                payloads = [part.split(b'\r\n\r\n', 1)[1][:-2] for part in parts]
                assert payloads == [bytes(range(12, 24)), bytes(range(12)), bytes(range(12, 24))]
                with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                    results = list(pool.map(lambda _: request(port, a + '/frames/2,1,2', accept), range(64)))
                assert all(result[0] == 200 for result in results), [result[0] for result in results]
                with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                    results = list(pool.map(lambda _: request(port, b + '/metadata', compact_type), range(32)))
                assert all(result[0] == 200 for result in results), [result[0] for result in results]
                assert request(port, a + '/frames/1')[0] == 406
                for quality in ['NaN', 'inf', '-1', '1.001', '0.1234', '1e0', '+1']:
                    assert request(port, a + '/frames/1', 'application/octet-stream;q=' + quality)[0] == 406
                assert request(port, a + '/frames/99', accept)[0] == 409
                assert request(port, '/dicom-web/studies/2.25.100/series/2.25.201/metadata')[0] == 200
                if exercise_deadline:
                    # A byte each second defeats an idle-only timeout. The absolute
                    # deadline must still close this incomplete request near 30 s.
                    slow = socket.create_connection(('127.0.0.1', port), timeout=3)
                    started = time.monotonic()
                    slow.settimeout(.1)
                    try:
                        while time.monotonic() - started < 34:
                            try:
                                slow.sendall(b'G')
                                if slow.recv(1) == b'':
                                    break
                            except socket.timeout:
                                pass
                            except (BrokenPipeError, ConnectionResetError):
                                break
                            time.sleep(.9)
                        elapsed = time.monotonic() - started
                        assert 28 <= elapsed < 33, elapsed
                        assert request(port, '/health')[0] == 200
                        print(f'Absolute slow-client deadline enforced after {elapsed:.1f} seconds.')
                    finally:
                        slow.close()
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
        print('Synthetic archive ingest/publication/HTTP regressions passed.')


def test_dicom_zip(binary, capture_directory=False):
    with tempfile.TemporaryDirectory(prefix='slidescape-dicom-zip-') as temp:
        root = Path(temp)
        repository = root / 'source-repository'
        a_path, b_path, c_path = [root / name for name in ('a.dcm', 'b.dcm', 'c.dcm')]
        a = write_slide(a_path, 1)
        b = write_slide(b_path, 2, series=201, sparse=True)
        c = write_slide(c_path, 3)
        b_pixels = bytes(range(100, 124))
        b_path.write_bytes(b_path.read_bytes()[:-24] + b_pixels)
        source = root / 'study.zip'
        with zipfile.ZipFile(source, 'w', compression=zipfile.ZIP_STORED) as bundle:
            # Force ZIP64 local headers on tiny members, with nested extensionless names.
            for name, path in [('nested/a', a_path), ('nested/b', b_path), ('other/c', c_path)]:
                with bundle.open(name, 'w', force_zip64=True) as member:
                    member.write(path.read_bytes())
            bundle.writestr('.sidecar', b'original metadata')
            bundle.writestr('empty/', b'')
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        run(binary, 'init', repository)
        tree = root / 'source'
        ingest_source, options = source, []
        expected = {}
        if capture_directory:
            # The chosen directory becomes the ZIP root; unrelated siblings stay outside.
            expected = {'nested/a': a_path.read_bytes(), 'nested/b': b_path.read_bytes(),
                        'other/c': c_path.read_bytes(), '.sidecar': b'original metadata',
                        '.hidden/notes.txt': b'hidden', 'métadonnées.txt': b'UTF-8 path', 'zero': b''}
            for name, data in expected.items():
                target = tree / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            (tree / 'empty').mkdir()
            ingest_source, options = tree, ['--zip-directory']
            for arguments in [options, ['--source', tree, *options, '--reference'],
                              ['--source', source, *options], ['--source', a_path, *options],
                              ['--source', tree / 'empty', *options]]:
                run(binary, 'ingest', repository, *arguments, success=False)
            if os.name != 'nt':
                alias = root / 'tree-link'
                alias.symlink_to(tree, target_is_directory=True)
                run(binary, 'ingest', repository, '--source', str(alias) + '/', *options, success=False)
                alias.unlink()
            for containing in (root, repository):
                failed = run(binary, 'ingest', repository, '--source', containing, *options, success=False)
                assert 'destination repository' in failed.stderr, failed.stderr
        run(binary, 'ingest', repository, '--source', ingest_source, *options)
        if capture_directory:
            # Timestamps and the source root's name are not part of the capture identity.
            os.utime(tree / 'nested/a', (1000000000, 1000000000))
            renamed = root / 'renamed-source'
            tree.rename(renamed)
            ingest_source = tree = renamed
        run(binary, 'ingest', repository, '--source', ingest_source, *options)
        with sqlite3.connect(repository / 'archive.sqlite3') as database:
            scalar = lambda sql: database.execute(sql).fetchone()[0]
            assert scalar('SELECT COUNT(*) FROM publication') == 2
            assert scalar('SELECT COUNT(*) FROM instance') == 3
            assert scalar('SELECT COUNT(*) FROM bundle') == 1
            assert scalar('SELECT COUNT(*) FROM bundle_object') == 1
            assert scalar("SELECT COUNT(*) FROM object WHERE media_type='application/dicom'") == 0
            key = scalar("SELECT storage_key FROM object WHERE media_type='application/zip'")
            shells = [repository / row[0] for row in database.execute(
                "SELECT storage_key FROM object WHERE media_type='application/vnd.slidescape.virtual-dicom-shell'")]
            if capture_directory:
                digest = hashlib.sha256((repository / key).read_bytes()).hexdigest()
                with zipfile.ZipFile(repository / key) as captured:
                    assert captured.testzip() is None
                    assert all(info.compress_type == zipfile.ZIP_STORED for info in captured.infolist())
                    assert captured.namelist() == sorted(captured.namelist())
                    directories = {'nested/', 'other/', '.hidden/', 'empty/'}
                    assert set(captured.namelist()) == set(expected) | directories
                    for name, data in expected.items():
                        assert captured.read(name) == data, name
                # Changed tree: reject replacement and do not partially publish a new series.
                write_slide(tree / 'new-instance', 5, series=200)
                run(binary, 'ingest', repository, '--source', tree, *options, success=False)
                (tree / 'new-instance').unlink()
                if os.name != 'nt':
                    (tree / 'link').symlink_to(a_path)
                    failed = run(binary, 'ingest', repository, '--source', tree, *options, success=False)
                    assert 'links' in failed.stderr, failed.stderr
                    (tree / 'link').unlink()
                assert all(path.name == 'objects' and path.is_dir() and
                           not list(path.iterdir())
                           for path in (repository / 'staging').iterdir())
            assert scalar("SELECT bundle_digest FROM bundle WHERE state='PUBLISHED'") == digest
            # Reject incomplete member payloads even when later ZIP bytes are available.
            write_slide(c_path, 4, truncated=True)
            damaged = root / 'damaged.zip'
            with zipfile.ZipFile(damaged, 'w') as bundle:
                bundle.write(c_path, 'bad.dcm')
                bundle.writestr('padding', bytes(4096))
            run(binary, 'ingest', repository, '--source', damaged, success=False)
            # A header-only Part 10 member is malformed, not an ignorable sidecar.
            with zipfile.ZipFile(damaged, 'w') as bundle:
                bundle.write(a_path, 'valid')
                bundle.writestr('incomplete', bytes(128) + b'DICM')
            run(binary, 'ingest', repository, '--source', damaged, success=False)
            # Reject ambiguous SOP identities without requiring per-member hashes.
            with zipfile.ZipFile(damaged, 'w') as bundle:
                bundle.write(a_path, 'one')
                bundle.write(a_path, 'two')
            run(binary, 'ingest', repository, '--source', damaged, success=False)
            # A later series conflict rolls back an earlier new series in the same ZIP.
            write_slide(c_path, 5, series=200)
            with zipfile.ZipFile(damaged, 'w') as bundle:
                bundle.write(c_path, 'new')
                bundle.write(a_path, 'conflicting')
            run(binary, 'ingest', repository, '--source', damaged, success=False)
            assert scalar('SELECT COUNT(*) FROM publication') == 2
            assert scalar('SELECT COUNT(*) FROM instance') == 3
        source.unlink()
        if capture_directory:
            shutil.rmtree(tree)
        for path in (a_path, b_path, c_path):
            path.unlink()
        owned = repository / key
        hidden = owned.with_suffix('.hidden')
        owned.rename(hidden)
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        log = root / 'server.log'
        with log.open('w+') as output:
            server = subprocess.Popen([binary, 'serve', str(repository), '--port', str(port)], stdout=output, stderr=output)
            try:
                deadline = time.monotonic() + 10
                while 'listening on' not in log.read_text():
                    assert server.poll() is None, log.read_text()
                    assert time.monotonic() < deadline, log.read_text()
                    time.sleep(.02)
                for path in (a, b, c):
                    assert request(port, path + '/metadata')[0] == 200
                status, _, body = request(port, a.rsplit('/instances/', 1)[0] + '/metadata')
                assert status == 200 and len(json.loads(body)) == 2
                hidden.rename(owned)
                accept = 'multipart/related; type="application/octet-stream"; transfer-syntax=' + TRANSFER_SYNTAX
                def check_frames(path):
                    status, headers, body = request(port, path + '/frames/2,1,2', accept)
                    assert status == 200, body
                    boundary = headers['Content-Type'].split('boundary=')[1].encode()
                    parts = body.split(b'--' + boundary)[1:-1]
                    pixels = b_pixels if path == b else bytes(range(24))
                    assert [part.split(b'\r\n\r\n', 1)[1][:-2] for part in parts] == [pixels[12:], pixels[:12], pixels[12:]]
                with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                    list(pool.map(check_frames, [a, b, c] * 8))
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
        # Cold-process repair must produce byte-identical published shells.
        shell_digests = {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in shells}
        for path in shells:
            path.unlink()
        with log.open('w+') as output:
            server = subprocess.Popen([binary, 'serve', str(repository), '--port', str(port)], stdout=output, stderr=output)
            try:
                deadline = time.monotonic() + 10
                while 'listening on' not in log.read_text():
                    assert server.poll() is None, log.read_text()
                    assert time.monotonic() < deadline, log.read_text()
                    time.sleep(.02)
                for path in (a, b, c):
                    status, _, body = request(port, path + '/metadata')
                    assert status == 200, body
                for path, expected in shell_digests.items():
                    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
        print(f'DICOM ZIP ownership, publication, ordered frames, concurrency, and shell repair passed (capture={capture_directory}).')


if __name__ == '__main__':
    main(str(Path(sys.argv[1]).resolve()), '--slow' in sys.argv[2:])
    test_dicom_zip(str(Path(sys.argv[1]).resolve()))
    test_dicom_zip(str(Path(sys.argv[1]).resolve()), capture_directory=True)
