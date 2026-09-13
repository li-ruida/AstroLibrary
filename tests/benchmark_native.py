"""Reproducible CPU/preview benchmark. Creates only temporary synthetic FIT data."""
import array
import http.client
import json
import os
from pathlib import Path
import platform
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from fixture_helpers import png_pixels

BINARY = ROOT / 'build/native/astrolibrary'

def main():
    with tempfile.TemporaryDirectory(prefix='astrolibrary-benchmark-') as temporary:
        root = Path(temporary).resolve()
        width, height = 1920, 1080
        source = root/'synthetic.fit'
        header = {'SIMPLE':'T','BITPIX':'-32','NAXIS':'3','NAXIS1':str(width),'NAXIS2':str(height),'NAXIS3':'3'}
        cards = ''.join(f'{key:8}= {value}'.ljust(80) for key,value in header.items())+'END'.ljust(80)
        with source.open('wb') as stream:
            stream.write(cards.encode().ljust(2880,b' '))
            for c in range(3):
                values = array.array('f', (.014+c*.003+((i*7919)%65521)/65521*.008+(0.8 if i%997==0 else 0) for i in range(width*height)))
                if sys.byteorder=='little': values.byteswap()
                values.tofile(stream)
        # Warm OS pages. These are NOT cold HDD timings.
        subprocess.run([BINARY,'render','--input',source,'--output',root/'warmup.png'],check=True,capture_output=True)
        expected = (root/'warmup.png').read_bytes()
        native, decode = [], []
        for run in range(3):
            output=root/f'native-{run}.png'
            start=time.perf_counter()
            p=subprocess.run([BINARY,'render','--input',source,'--output',output],check=True,capture_output=True,text=True)
            native.append((time.perf_counter()-start)*1000)
            decode.append(json.loads(p.stdout)['decodeMs'])
            assert png_pixels(output.read_bytes())==png_pixels(expected)
        with socket.socket() as s:
            s.bind(('127.0.0.1',0));port=s.getsockname()[1]
        log=open(root/'server.log','w')
        process=subprocess.Popen([BINARY,'serve','--library',root,'--data-dir',root/'settings','--cache-dir',root/'cache','--web-root',ROOT/'web','--port',str(port)],env=dict(os.environ,PATH='/nonexistent'),stdout=log,stderr=log)
        def request(path):
            conn=http.client.HTTPConnection('127.0.0.1',port,timeout=30);start=time.perf_counter();conn.request('GET',path);r=conn.getresponse();b=r.read();elapsed=(time.perf_counter()-start)*1000;conn.close();assert r.status==200,(r.status,b);return elapsed,b
        try:
            for _ in range(200):
                try: request('/api/health');break
                except OSError: time.sleep(.02)
            first,png=request('/api/fits-preview/synthetic.fit')
            cached=[request('/api/fits-preview/synthetic.fit')[0] for _ in range(10)]
            assert png_pixels(png)==png_pixels(expected)
        finally:
            process.terminate();process.wait(10);log.close()
        result={'fixture':{'width':width,'height':height,'channels':3,'bitpix':-32,'sourceBytes':source.stat().st_size,'previewWidth':960,'previewHeight':540},'platform':platform.platform(),'method':'3 runs, median, warmed OS file cache. C++ render includes process startup and PNG file write; HTTP cold means empty AstroLibrary cache, not cold physical disk.','nativeRenderWallMs':round(statistics.median(native),2),'nativeDecodeMs':round(statistics.median(decode),2),'nativeFirstHTTPPreviewMs':round(first,2),'nativeCachedHTTPPreviewMs':round(statistics.median(cached),2),'cliHTTPPixelsIdentical':True}
        print(json.dumps(result,ensure_ascii=False,indent=2))

if __name__=='__main__':main()
