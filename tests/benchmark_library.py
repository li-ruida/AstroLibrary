"""Measure the native library API on synthetic SUB-heavy data, without user files.

Each run starts with an empty App cache. File pages are warm from fixture writes;
this measures software overhead, not cold mechanical disk throughput.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import os
from pathlib import Path
import socket
import statistics
import subprocess
import tempfile
import time

from fixture_helpers import write_fits

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=ROOT/'build/native/astrolibrary')
    parser.add_argument('--frames', type=int, default=6000)
    args = parser.parse_args()
    assert args.frames >= 20
    with tempfile.TemporaryDirectory(prefix='astrolibrary-library-benchmark-') as temporary:
        root = Path(temporary).resolve()
        gallery = root/'gallery'
        gallery.mkdir()
        seed = root/'seed.fit'
        write_fits(seed, [.02, .021, .03, .5], bitpix=-32,
                   extra={'DATE-OBS': "'2026-08-30T14:00:00'", 'CREATOR': "'ZWO Seestar S50'"})
        data = seed.read_bytes()
        for target in range(20):
            master = gallery/f'Target {target:02}'
            sub = gallery/f'Target {target:02}_sub'
            master.mkdir(); sub.mkdir()
            for n in range(3):
                (master/f'master-{n}.fit').write_bytes(data)
            for n in range(target, args.frames, 20):
                (sub/f'frame-{n:05}.fit').write_bytes(data)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        with (root/'server.log').open('w') as log:
            process = subprocess.Popen([args.binary.resolve(), 'serve', '--library', gallery,
                '--data-dir', root/'settings', '--cache-dir', root/'cache', '--web-root', ROOT/'web',
                '--port', str(port)], env=dict(os.environ, PATH='/nonexistent'), stdout=log, stderr=log)
            def request(path):
                connection = http.client.HTTPConnection('127.0.0.1', port, timeout=120)
                start = time.perf_counter()
                try:
                    connection.request('GET', path)
                    response = connection.getresponse()
                    body = response.read()
                    elapsed = (time.perf_counter()-start)*1000
                    assert response.status == 200, (response.status, body[:300])
                    return elapsed, json.loads(body), len(body)
                finally:
                    connection.close()
            try:
                deadline = time.monotonic()+10
                while True:
                    assert process.poll() is None, 'server exited'
                    try:
                        request('/api/health')
                        break
                    except OSError:
                        if time.monotonic() > deadline:
                            raise
                        time.sleep(.02)
                cold, library, payload_size = request('/api/library')
                assert library['stats']['subRaw'] == args.frames
                assert len(library['items']) == 60
                assert all('items' not in group for col in library['subCollections'].values() for group in col['groups'])
                refresh = request('/api/library?refresh=1')[0]
                warm = [request('/api/library')[0] for _ in range(9)]
                sub_path = '/api/sub-collection?target=Target%2000'
                sub = [request(sub_path)[0] for _ in range(9)]
                assert request(sub_path)[1]['collection']['raw'] == (args.frames+19)//20
                status = [request('/api/fits-cache')[0] for _ in range(9)]
                with ThreadPoolExecutor(max_workers=8) as pool:
                    concurrent = list(pool.map(lambda _: request('/api/library')[0], range(8)))
                rss = int(subprocess.check_output(['/bin/ps', '-o', 'rss=', '-p', str(process.pid)]).strip())
                result = {'fixture': {'masters': 60, 'subFrames': args.frames, 'targets': 20},
                    'method': 'Warm OS pages; empty App cache for first scan; medians of 9 warm API requests; 8 concurrent warm library requests.',
                    'coldLibraryMs': round(cold, 2), 'refreshLibraryMs': round(refresh, 2),
                    'warmLibraryMs': round(statistics.median(warm), 2),
                    'targetSubMs': round(statistics.median(sub), 2),
                    'cacheStatusMs': round(statistics.median(status), 2),
                    'concurrentLibraryMedianMs': round(statistics.median(concurrent), 2),
                    'residentKiB': rss, 'libraryPayloadBytes': payload_size}
                print(json.dumps(result, indent=2))
            finally:
                process.terminate()
                try:
                    process.wait(8)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait()


if __name__ == '__main__':
    main()
