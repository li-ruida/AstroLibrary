"""Black-box tests of the shipped C++ executable; Python only drives the test harness; expectations are frozen fixtures.

Every server uses disposable settings/cache/library directories. No user library
is mutated. The Trash test moves only a uniquely named fixture created here.
"""
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import http.client
import json
import os
from pathlib import Path
import socket
import sqlite3
import struct
import subprocess
import tempfile
import time
import unittest
import uuid

from fixture_helpers import write_fits, png_pixels

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/native/astrolibrary"
FIXTURES = ROOT / "tests/fixtures"
REFERENCE = json.loads((FIXTURES / "api-reference.json").read_text())

def expected_pixels(record):
    return record["width"], record["height"], bytes.fromhex(record["pixelsHex"])


class NativeFitsTests(unittest.TestCase):
    def test_native_pixels_match_frozen_reference_across_formats_and_controls(self):
        cases = json.loads((FIXTURES / 'fits-reference.json').read_text())['cases']
        self.assertEqual(len(cases), 96)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for index, case in enumerate(cases):
                with self.subTest(case=index, options=case['options']):
                    source, output = root / f'{index}.fit', root / f'{index}.png'
                    original = base64.b64decode(case['fitsBase64'])
                    source.write_bytes(original)
                    options = case['options']
                    args = [BINARY, 'render', '--input', source, '--output', output,
                            '--mode', options['mode'], '--black', str(options['black']),
                            '--brightness', str(options['brightness'])]
                    if options['neutralize']:
                        args.append('--neutralize')
                    run = subprocess.run(args, capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stderr)
                    self.assertEqual(png_pixels(output.read_bytes()), expected_pixels(case))
                    self.assertEqual(source.read_bytes(), original)

    def test_invalid_fit_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            source, output = Path(tmp)/'broken.fit', Path(tmp)/'out.png'
            source.write_bytes(b'not FITS')
            result = subprocess.run([BINARY, 'render', '--input', source, '--output', output], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())
            write_fits(source, [0, 1, 2, 3])
            output.write_bytes(b'keep')
            result = subprocess.run([BINARY, 'render', '--input', source, '--output', output], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), b'keep')


class NativeAPITests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='astrolibrary-native-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.library = self.root/'gallery'
        self.seestar = self.root/'seestar'
        self.edited = self.root/'edited'
        self.data = self.root/'data'
        for p in (self.library/'M 31 - 仙女座', self.library/'M 31_sub', self.library/'MilkyWay', self.seestar, self.edited/'M 31', self.data):
            p.mkdir(parents=True, exist_ok=True)
        self.fit = self.library/'M 31 - 仙女座'/'image_20260830-220000.fit'
        write_fits(self.fit, [.02, .021, .03, .5], bitpix=-32, extra={'DATE-OBS': "'2026-08-30T14:00:00'", 'CREATOR': "'ZWO Seestar S50'", 'FILTER': "'LP'"})
        write_fits(self.library/'M 31_sub'/'sub.fit', [.01, .02, .03, .5], bitpix=-32)
        (self.fit.with_suffix('.jpg')).write_bytes(b'test-jpeg')
        (self.edited/'M 31'/'final.jpg').write_bytes(b'edited-jpeg')
        self.token = uuid.uuid4().hex
        self.log = open(self.root/'server.log', 'w+')
        self.addCleanup(self.log.close)
        self.start_server()
        self.addCleanup(self.stop_server)
        self.csrf = self.request('GET', '/api/auth/status')[1]['csrfToken']

    def start_server(self):
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            self.port = s.getsockname()[1]
        env = dict(os.environ, ASTROLIBRARY_APP_TOKEN=self.token, PATH='/nonexistent')
        self.process = subprocess.Popen([BINARY, 'serve', '--library', self.library, '--data-dir', self.data, '--cache-dir', self.root/'cache', '--web-root', ROOT/'web', '--port', str(self.port)], env=env, stdout=self.log, stderr=self.log)
        end = time.monotonic()+10
        while time.monotonic()<end:
            if self.process.poll() is not None:
                self.log.flush(); self.log.seek(0)
                self.fail(self.log.read())
            try:
                if self.request('GET', '/api/health')[0]==200:
                    return
            except OSError:
                pass
            time.sleep(.02)
        self.fail('native server startup timeout')

    def stop_server(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(8)
            except subprocess.TimeoutExpired:
                self.process.kill(); self.process.wait()

    def request(self, method, path, payload=None, headers=None, native=False, raw=False):
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=10)
        h = dict(headers or {})
        if native:
            h['X-AstroLibrary-App-Token'] = self.token
        if payload is not None:
            h['Content-Type'] = 'application/json'
        conn.request(method, path, None if payload is None else json.dumps(payload), h)
        response = conn.getresponse()
        body = response.read()
        result = (response.status, body if raw else json.loads(body or b'{}'), dict(response.getheaders()))
        conn.close()
        return result

    def post(self, path, data=None, native=True):
        status, body, _ = self.request('POST', path, {} if data is None else data, native=native)
        self.assertEqual(status, 200, body)
        return body

    def wait_job(self, path):
        end = time.monotonic()+10
        while time.monotonic()<end:
            status, body, _ = self.request('GET', path, native=True)
            self.assertEqual(status, 200, body)
            job = body.get('job', body)
            if not job['running']:
                return body
            time.sleep(.02)
        self.fail(f'job did not complete: {body}')

    def test_starts_without_python_and_protects_request_context(self):
        status, body, headers = self.request('GET', '/api/health')
        self.assertEqual(body['backend'], 'cpp')
        self.assertIn("frame-ancestors 'none'", headers['Content-Security-Policy'])
        self.assertEqual(self.request('GET', '/api/health', headers={'Host':'evil.example'})[0], 421)
        self.assertEqual(self.request('POST', '/api/settings', {})[0], 403)
        self.assertEqual(self.request('POST', '/api/settings', {}, headers={'Origin':'https://evil.example','X-AstroLibrary-CSRF':self.csrf})[0], 403)
        self.assertEqual(self.request('POST', '/api/settings', {}, headers={'X-AstroLibrary-CSRF':self.csrf})[0], 200)
        self.assertEqual(self.request('POST', '/api/settings', [], native=True)[0], 400)
        self.assertEqual(self.request('POST', '/api/settings', {'large':'x'*150000}, native=True)[0], 413)
        self.assertEqual(self.request('GET', '/api/app/health')[0], 403)

    def test_library_matches_reference_and_lazy_sub_groups(self):
        self.post('/api/settings', {'editedSource': str(self.edited)})
        _, library, _ = self.request('GET','/api/library')
        old = REFERENCE
        self.assertEqual(library['stats'], old['stats'])
        fields = ['id','category','kind','extension','size','isSub','capturedAt','captureDate','device','filter','exposure']
        self.assertEqual([{k:i[k] for k in fields} for i in sorted(library['items'], key=lambda i:i['id'])], [{k:i[k] for k in fields} for i in sorted(old['items'], key=lambda i:i['id'])])
        self.assertNotIn('items', library['subCollections']['M 31']['groups'][0])
        sub = self.request('GET','/api/sub-collection?target=M%2031')[1]['collection']
        self.assertEqual(sub['groups'][0]['items'][0]['ratingScope'], str(self.library))
        self.assertEqual(library['editedItems'][0]['category'], 'M 31')
        self.assertTrue(self.request('GET','/api/library')[1]['scan']['cached'])

    def test_rating_tags_and_workflow_persist_and_match_legacy_keys(self):
        item = self.request('GET','/api/library')[1]['items'][0]
        p = {'id': item['id'], 'scope':str(self.library), 'isEdited':False, 'rating':4}
        self.post('/api/rating',p)
        self.post('/api/tags', {'target':'M 31 - 仙女座','tags':['星系','收藏']})
        self.post('/api/workflow',{'target':'M 31','changes':{'favorite':True,'stage':'processing','notes':'原生迁移'}})
        key = hashlib.sha256(json.dumps([str(self.library), item['id']], ensure_ascii=True).encode()).hexdigest()
        saved = json.loads((self.data/'catalog.json').read_text())
        self.assertEqual(saved['photoRatings'][key], 4)
        self.stop_server(); self.start_server()
        library = self.request('GET','/api/library')[1]
        item = next(i for i in library['items'] if i['id']==p['id'])
        self.assertEqual(item['rating'], 4)
        self.assertIn('星系', item['tags'])
        self.assertTrue(library['targetWorkflow']['M 31']['favorite'])
        for invalid in (True, -1, 6, 1.5, '5'):
            self.assertEqual(self.request('POST','/api/rating',{**p,'rating':invalid},native=True)[0],400)
        self.assertEqual(self.request('POST','/api/rating',{**p,'scope':'/old'},native=True)[0],400)
        self.assertEqual(self.request('POST','/api/workflow',{'target':'M31','changes':{'stage':'oops'}},native=True)[0],400)

    def test_fit_preview_cache_etag_corruption_clear_and_switch(self):
        url = next(i['url'] for i in self.request('GET','/api/library')[1]['items'] if i['kind']=='raw')
        status, first, headers = self.request('GET',url,raw=True)
        self.assertEqual(status,200,first)
        self.assertEqual(png_pixels(first),expected_pixels(REFERENCE['preview']))
        etag = headers['ETag']
        self.assertEqual(self.request('GET',url,headers={'If-None-Match':etag},raw=True)[0],304)
        self.assertEqual(self.request('GET',url+'&black=nan',raw=True)[0],422)
        self.assertEqual(self.request('GET',url+'&neutralize=2',raw=True)[0],422)
        self.stop_server()
        db = self.root/'cache/fits-v1.sqlite3'
        with sqlite3.connect(db) as conn:
            before = conn.execute('SELECT count(*) FROM entries').fetchone()[0]
        self.assertGreaterEqual(before,3)
        self.start_server()
        self.assertEqual(self.request('GET',url,raw=True)[1],first)
        self.stop_server()
        with sqlite3.connect(db) as conn:
            conn.execute("UPDATE entries SET value=x'00',checksum='bad'")
        self.start_server()
        self.assertEqual(self.request('GET',url,raw=True)[1],first)
        cache_parent = self.root/'fast-disk';cache_parent.mkdir()
        self.post('/api/fits-cache/directory',{'directory':str(cache_parent)})
        state = self.request('GET','/api/fits-cache')[1]
        self.assertEqual(state['configuredDirectory'],str(cache_parent/'AstroLibrary-FIT-Cache'))
        self.post('/api/fits-cache/rebuild')
        done = self.wait_job('/api/fits-cache')
        self.assertEqual(done['job']['phase'],'completed',done)
        self.assertEqual(done['job']['processed'],1)
        self.assertEqual(done['job']['failed'],0,done)
        self.post('/api/fits-cache/clear')
        self.assertEqual(self.wait_job('/api/fits-cache')['cache']['entries'],0)
        self.post('/api/fits-cache/directory',{'directory':''})
        self.assertEqual(self.request('GET','/api/fits-cache')[1]['configuredDirectory'],'')

    def test_cache_rebuild_skips_nested_sub_but_keeps_on_demand_preview(self):
        sub = self.library/'nested'/'M 42_SuB'/'deeper'
        sub.mkdir(parents=True)
        (sub/'invalid.fit').write_bytes(b'must not be parsed by cache rebuild')
        self.post('/api/fits-cache/rebuild')
        done = self.wait_job('/api/fits-cache')
        self.assertEqual(done['job']['phase'], 'completed', done)
        self.assertEqual(done['job']['total'], 1)
        self.assertEqual(done['job']['processed'], 1)
        self.assertEqual(done['job']['failed'], 0, done)
        entries = done['cache']['entries']
        status, preview, _ = self.request('GET', '/api/fits-preview/M%2031_sub/sub.fit', raw=True)
        self.assertEqual(status, 200, preview)
        self.assertTrue(preview.startswith(b'\x89PNG\r\n\x1a\n'))
        self.assertGreater(self.request('GET', '/api/fits-cache')[1]['cache']['entries'], entries)

    def test_reuses_python_sample_cache_without_decoding_again(self):
        self.stop_server()
        # Inject bytes produced by the removed Python implementation, rebinding
        # only the fingerprint to this disposable source file.
        stat = self.fit.stat()
        identity = [str(self.fit.resolve()), stat.st_dev, stat.st_ino, stat.st_size,
                    stat.st_mtime_ns, stat.st_ctime_ns]
        key = hashlib.sha256(json.dumps(['sample', [1, identity]], separators=(',', ':'), ensure_ascii=True).encode()).hexdigest()
        sample = base64.b64decode(REFERENCE['legacySampleBase64'])
        (self.root/'cache').mkdir(exist_ok=True)
        with sqlite3.connect(self.root/'cache/fits-v1.sqlite3') as db:
            db.execute('CREATE TABLE IF NOT EXISTS entries (key TEXT PRIMARY KEY, kind TEXT NOT NULL, value BLOB NOT NULL, checksum TEXT NOT NULL, size INTEGER NOT NULL, touched REAL NOT NULL)')
            db.execute('DELETE FROM entries')
            db.execute('INSERT INTO entries VALUES(?,?,?,?,?,?)', (key, 'sample', sample, hashlib.sha256(sample).hexdigest(), len(sample), time.time()))
            db.execute("CREATE TRIGGER no_resample BEFORE INSERT ON entries WHEN NEW.kind='sample' BEGIN SELECT RAISE(ABORT, 'unexpected resample'); END")
        self.start_server()
        url='/api/fits-preview/'+__import__('urllib.parse',fromlist=['quote']).quote(str(self.fit.relative_to(self.library)))+'?brightness=1'
        status, actual, _=self.request('GET',url,raw=True)
        self.assertEqual(status,200,actual)
        self.assertEqual(png_pixels(actual),expected_pixels(REFERENCE['brightPreview']))
        self.assertEqual(self.request('GET','/api/fits-cache')[1]['cache']['error'],'')

    def test_unicode_password_compatibility_and_legacy_iteration_upgrade(self):
        password='星空'*300
        self.post('/api/app/auth',{'enabled':True,'username':'天文用户','password':password})
        self.assertEqual(self.request('POST','/api/auth/login',{'username':'天文用户','password':password},headers={'X-AstroLibrary-CSRF':self.csrf})[0],200)
        self.stop_server()
        path=self.data/'settings.json';saved=json.loads(path.read_text())
        record=dict(REFERENCE['legacyPassword'])
        record['iterations']=1000
        record['hash']=hashlib.pbkdf2_hmac('sha256',b'legacy-password',bytes.fromhex(record['salt']),1000).hex()
        saved['auth']={'enabled':True,'username':'legacy','password':record}
        path.write_text(json.dumps(saved))
        self.start_server()
        csrf=self.request('GET','/api/auth/status')[1]['csrfToken']
        self.assertEqual(self.request('POST','/api/auth/login',{'username':'legacy','password':'legacy-password'},headers={'X-AstroLibrary-CSRF':csrf})[0],200)
        self.assertEqual(json.loads(path.read_text())['auth']['password']['iterations'],600000)

    def test_export_rejects_symlink_parents_before_creating_directories(self):
        dest=self.root/'export';dest.mkdir()
        outside=self.root/'outside';outside.mkdir()
        (self.library/'link/nested').mkdir(parents=True)
        (self.library/'link/nested/image.jpg').write_bytes(b'fixture')
        (dest/'link').symlink_to(outside,target_is_directory=True)
        result=subprocess.run([BINARY,'export','-s',self.library,'-d',dest],capture_output=True)
        self.assertNotEqual(result.returncode,0)
        self.assertFalse((outside/'nested').exists())

    def test_library_switch_warms_new_cache_and_preserves_offline_selection(self):
        new=self.root/'new';new.mkdir();write_fits(new/'new.fit',[0,1,2,3])
        sub=new/'nested'/'M 42_SUB';sub.mkdir(parents=True)
        (sub/'invalid.fit').write_bytes(b'must not be parsed by cache warmup')
        self.post('/api/settings',{'libraryPath':str(new)})
        job=self.wait_job('/api/fits-cache')['job']
        self.assertEqual(job['operation'],'warmup')
        self.assertEqual(job['source'],str(new))
        self.assertEqual(job['processed'],1)
        self.assertEqual(job['total'],1)
        self.assertEqual(job['failed'],0,job)
        missing=self.root/'offline'
        self.post('/api/settings',{'libraryPath':str(missing)})
        self.wait_job('/api/fits-cache')
        library=self.request('GET','/api/library')[1]
        self.assertEqual(library['source'],str(missing))
        self.assertEqual(library['items'],[])
        self.assertFalse(library['sourceStatus']['accessible'])

    def test_auth_hash_compatibility_sessions_and_native_only_configuration(self):
        self.post('/api/app/auth',{'enabled':True,'username':'astro','password':'native-password'})
        record=json.loads((self.data/'settings.json').read_text())['auth']['password']
        self.assertEqual(record['algorithm'], 'pbkdf2_sha256')
        self.assertEqual(record['iterations'], 600000)
        self.assertEqual(hashlib.pbkdf2_hmac('sha256', b'native-password', bytes.fromhex(record['salt']), record['iterations']).hex(), record['hash'])
        self.assertEqual(self.request('GET','/api/library')[0],401)
        status,body,h=self.request('POST','/api/auth/login',{'username':'astro','password':'native-password'},headers={'X-AstroLibrary-CSRF':self.csrf})
        self.assertEqual(status,200,body)
        cookie=h['Set-Cookie'].split(';')[0]
        self.assertEqual(self.request('GET','/api/library',headers={'Cookie':cookie})[0],200)
        self.assertTrue(self.request('GET','/api/auth/status',headers={'Cookie':cookie})[1]['authenticated'])
        self.assertEqual(self.request('POST','/api/app/auth',{'enabled':False},headers={'Cookie':cookie,'X-AstroLibrary-CSRF':self.csrf})[0],403)
        fiturl=next(i['url'] for i in self.request('GET','/api/library',native=True)[1]['items'] if i['kind']=='raw')
        self.assertEqual(self.request('GET',fiturl,native=True,raw=True)[2]['Cache-Control'],'no-store')
        self.request('POST','/api/auth/logout',{},headers={'Cookie':cookie,'X-AstroLibrary-CSRF':self.csrf})
        self.assertEqual(self.request('GET','/api/library',headers={'Cookie':cookie})[0],401)
        self.post('/api/app/auth',{'enabled':False})
        self.assertEqual(self.request('GET','/api/library')[0],200)

    def test_import_never_overwrites_and_seestar_cleanup_checks_full_copy(self):
        (self.seestar/'new.jpg').write_bytes(b'complete-copy')
        (self.seestar/'different.jpg').write_bytes(b'source')
        (self.library/'different.jpg').write_bytes(b'existing')
        (self.seestar/'outside.jpg').symlink_to(self.fit)
        self.post('/api/settings',{'seestarSource':str(self.seestar)})
        self.post('/api/seestar/import')
        job=self.wait_job('/api/seestar/import')
        self.assertEqual(job['imported'],1,job)
        self.assertEqual(job['skipped'],1,job)
        self.assertEqual((self.library/'different.jpg').read_bytes(),b'existing')
        self.assertFalse((self.library/'outside.jpg').exists())
        self.post('/api/seestar/cleanup/preview')
        job=self.wait_job('/api/seestar/cleanup')
        self.assertEqual(job['phase'],'ready',job)
        self.assertEqual(job['count'],1)
        (self.library/'new.jpg').write_bytes(b'changed-after-preview')
        self.post('/api/seestar/cleanup/execute',{'token':job['token']})
        job=self.wait_job('/api/seestar/cleanup')
        self.assertEqual(job['moved'],0)
        self.assertTrue((self.seestar/'new.jpg').exists())

    def test_target_cleanup_scope_fingerprint_and_single_use(self):
        context={'target':'M 31','source':str(self.library),'mergeTargets':True}
        result=self.post('/api/cleanup-target/preview',context)
        job=self.wait_job('/api/cleanup-target')
        self.assertEqual(job['phase'],'ready',job)
        self.assertEqual((job['masters'],job['subs'],job['count']),(2,1,3))
        for p in (self.fit,self.fit.with_suffix('.jpg'),self.library/'M 31_sub/sub.fit'):
            p.write_bytes(p.read_bytes()+b'changed')
        self.post('/api/cleanup-target/execute',{**context,'token':job['token'],'jobId':result['jobId']})
        done=self.wait_job('/api/cleanup-target')
        self.assertEqual(done['moved'],0)
        self.assertEqual(done['skipped'],3)
        self.assertEqual(self.request('POST','/api/cleanup-target/execute',{**context,'token':job['token'],'jobId':result['jobId']},native=True)[0],400)
        self.assertTrue((self.edited/'M 31/final.jpg').exists())

    def test_jpg_cleanup_only_moves_confirmed_unchanged_fixture_to_trash(self):
        # Only this uniquely named test fixture may be moved. Existing fixture JPGs
        # are altered after preview so fingerprint validation must preserve them.
        name='astrolibrary-native-trash-test-'+uuid.uuid4().hex+'.JPG'
        test=self.library/name;test.write_bytes(b'generated disposable fixture')
        plan=self.post('/api/cleanup-jpg/preview')
        original=self.fit.with_suffix('.jpg');original.write_bytes(b'changed')
        late=self.library/'late.jpg';late.write_bytes(b'keep added after preview')
        result=self.post('/api/cleanup-jpg',{'token':plan['token']})
        self.assertEqual(result['moved'],1,result)
        self.assertFalse(test.exists())
        self.assertTrue(original.exists());self.assertTrue(late.exists());self.assertTrue(self.fit.exists())
        trash=Path.home()/'.Trash'/name
        if trash.is_file() and trash.read_bytes()==b'generated disposable fixture':
            trash.unlink()

    def test_path_traversal_and_external_symlinks_excluded(self):
        outside=self.root/'secret.jpg';outside.write_bytes(b'private')
        (self.library/'escape.jpg').symlink_to(outside)
        (self.library/'escape-dir').symlink_to(self.edited,target_is_directory=True)
        body=self.request('GET','/api/library?refresh=1')[1]
        self.assertFalse(any(i['id'].startswith('escape') for i in body['items']))
        for url in ('/media/escape.jpg','/media/%2e%2e/secret.jpg','/assets/%2e%2e/settings.json'):
            self.assertNotEqual(self.request('GET',url,raw=True)[0],200)

    def test_r2_validation_and_secret_not_exposed(self):
        c={'enabled':True,'autoUpload':False,'accountId':'a'*32,'bucket':'astro-bucket','accessKeyId':'A'*20,'secretAccessKey':'secret-value-123456789','publicBaseUrl':'https://images.example.com','keyPrefix':'astro/previews'}
        result=self.post('/api/settings',{'r2':c})
        self.assertTrue(result['r2']['secretConfigured'])
        self.assertNotIn('secretAccessKey',result['r2'])
        self.assertNotIn(c['secretAccessKey'],json.dumps(result))
        c['secretAccessKey']=''
        self.post('/api/settings',{'r2':c})
        saved=json.loads((self.data/'settings.json').read_text())
        self.assertEqual(saved['r2']['secretAccessKey'],'secret-value-123456789')
        for change in ({'publicBaseUrl':'http://localhost:1234'},{'keyPrefix':'a/../b'},{'accountId':'evil.example.com'}):
            self.assertEqual(self.request('POST','/api/settings',{'r2':{**c,**change}},native=True)[0],400)

    def test_invalid_saved_settings_fail_closed_without_overwrite(self):
        self.stop_server()
        path=self.data/'settings.json'
        for contents in ('{broken', json.dumps({'auth':{'enabled':True,'username':'astro','password':{}}}), json.dumps({'port':True})):
            path.write_text(contents)
            result=subprocess.run([BINARY,'serve','--data-dir',self.data,'--cache-dir',self.root/'cache','--web-root',ROOT/'web'],capture_output=True,timeout=5)
            self.assertNotEqual(result.returncode,0)
            self.assertEqual(path.read_text(),contents)

    def test_cli_export_dry_run_and_overlapping_roots(self):
        dest=self.root/'export'
        base=[BINARY,'export','-s',self.library,'-d',dest,'--quiet']
        run=subprocess.run(base+['--dry-run'],capture_output=True,text=True)
        self.assertEqual(run.returncode,0,run.stderr);self.assertFalse(dest.exists())
        for expected in (3,0):
            run=subprocess.run(base,capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stderr)
            self.assertEqual(json.loads(run.stdout)['exported'],expected)
        run=subprocess.run([BINARY,'export','-s',self.library,'-d',self.library/'nested'],capture_output=True)
        self.assertNotEqual(run.returncode,0)

    def test_cli_help_and_export_selection(self):
        help_text = subprocess.run([BINARY, '--help'], capture_output=True, text=True, check=True).stdout
        for command in ('serve', 'export', 'auth', 'render'):
            self.assertIn(command, help_text)
        for options, wanted in ((['--single-only'], 2), (['--jpg-only'], 1)):
            dest = self.root/options[0][2:]
            result = subprocess.run([BINARY, 'export', '-s', self.library, '-d', dest,
                                     '--data-dir', self.data, '--cache-dir', self.root/'cache',
                                     '--quiet', *options], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)['exported'], wanted)
            self.assertFalse((dest/'M 31_sub/sub.fit').exists())

    def test_invalid_settings_batch_keeps_disk_and_memory_unchanged(self):
        self.post('/api/settings', {'mergeTargets':True})
        saved = (self.data/'settings.json').read_bytes()
        for invalid in ({'port':True}, {'port':80}, {'mergeTargets':'yes'},
                        {'galleryMode':'invalid'}, {'libraryPath':[]}, {'auth':{}},
                        {'fitsCacheDirectory':'/tmp'}, {'exportScope':'invalid'}):
            with self.subTest(invalid=invalid):
                result = self.request('POST', '/api/settings', {'editedSource':str(self.edited), **invalid}, native=True)
                self.assertEqual(result[0], 400, result)
                self.assertEqual((self.data/'settings.json').read_bytes(), saved)
                self.assertEqual(self.request('GET', '/api/settings')[1]['editedSource'], '')

    def test_failed_settings_write_preserves_active_library(self):
        self.post('/api/settings', {})
        path = self.data/'settings.json'
        saved = path.read_bytes()
        path.unlink(); path.mkdir()
        try:
            self.assertEqual(self.request('POST', '/api/settings', {'libraryPath':str(self.edited)}, native=True)[0], 400)
            self.assertEqual(self.request('GET', '/api/settings')[1]['source'], str(self.library))
        finally:
            path.rmdir(); path.write_bytes(saved)

    def test_workflow_partial_concurrent_updates_and_private_persistence(self):
        self.post('/api/tags', {'target':'M 31', 'tags':['窄带']})
        changes = [{'favorite':True}, {'stage':'done'}, {'notes':'完成'}]
        with ThreadPoolExecutor(max_workers=3) as pool:
            results = list(pool.map(lambda change: self.request('POST', '/api/workflow',
                {'target':'M 31 - 仙女座', 'changes':change}, native=True), changes))
        self.assertTrue(all(result[0] == 200 for result in results), results)
        self.post('/api/workflow', {'target':'SH2-103', 'changes':{'stage':'done'}})
        self.post('/api/workflow', {'target':'SH2-157', 'changes':{'favorite':True}})
        path = self.data/'catalog.json'
        saved = json.loads(path.read_text())
        self.assertEqual(saved['targetWorkflow']['M 31'], {'favorite':True, 'stage':'done', 'notes':'完成'})
        self.assertEqual(saved['targetTags']['M 31'], ['窄带'])
        self.assertEqual(len(saved['targetWorkflow']), 3)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        for invalid in (None, [], {}, {'favorite':1}, {'notes':'a'*4001}, {'unknown':True}):
            self.assertEqual(self.request('POST', '/api/workflow', {'target':'M 31', 'changes':invalid}, native=True)[0], 400)
            self.assertEqual(json.loads(path.read_text()), saved)

    def test_sub_and_edited_ratings_are_independent_and_clearable(self):
        self.post('/api/settings', {'editedSource':str(self.edited)})
        original = self.fit.read_bytes()
        paths = [(str(self.fit.relative_to(self.library)), self.library, False, 5),
                 ('M 31_sub/sub.fit', self.library, False, 2),
                 ('edited:M 31/final.jpg', self.edited, True, 3)]
        for name, scope, edited, rating in paths:
            self.post('/api/rating', {'id':name, 'scope':str(scope), 'isEdited':edited, 'rating':rating})
        library = self.request('GET', '/api/library')[1]
        self.assertEqual(next(i['rating'] for i in library['items'] if i['kind']=='raw'), 5)
        self.assertEqual(library['editedItems'][0]['rating'], 3)
        sub = self.request('GET', '/api/sub-collection?target=M%2031')[1]['collection']
        self.assertEqual(sub['groups'][0]['items'][0]['rating'], 2)
        for name, scope, edited, _ in paths:
            self.post('/api/rating', {'id':name, 'scope':str(scope), 'isEdited':edited, 'rating':0})
        self.assertEqual(json.loads((self.data/'catalog.json').read_text())['photoRatings'], {})
        self.assertEqual(self.fit.read_bytes(), original)

    def test_cache_directory_failures_keep_current_cache(self):
        parent = self.root/'ssd'; parent.mkdir()
        outside = self.root/'unrelated'; outside.mkdir()
        marker = outside/'keep.txt'; marker.write_bytes(b'keep')
        (parent/'AstroLibrary-FIT-Cache').symlink_to(outside, target_is_directory=True)
        before = self.request('GET', '/api/fits-cache')[1]
        for directory in (str(self.root/'missing'), str(self.fit), str(parent)):
            self.assertEqual(self.request('POST', '/api/fits-cache/directory', {'directory':directory}, native=True)[0], 400)
            after = self.request('GET', '/api/fits-cache')[1]
            self.assertEqual(after['configuredDirectory'], before['configuredDirectory'])
            self.assertEqual(after['cache']['path'], before['cache']['path'])
        self.assertEqual(list(outside.iterdir()), [marker])
        self.assertEqual(marker.read_bytes(), b'keep')

    def test_cache_accounting_external_writes_replacements_and_lru(self):
        db_path = self.root/'cache/fits-v1.sqlite3'
        def check_totals():
            status = self.request('GET', '/api/fits-cache')[1]['cache']
            with sqlite3.connect(db_path) as db:
                actual = db.execute('SELECT count(*),coalesce(sum(size),0) FROM entries').fetchone()
            self.assertEqual((status['entries'], status['bytes']), actual)
            return status
        check_totals()
        # Large declared sizes exercise the 2 GiB eviction threshold without
        # allocating large test files. External commits model another writer.
        with sqlite3.connect(db_path) as db:
            for key, size, touched in [('old', 2147483600, 0), ('recent', 40, time.time())]:
                value = key.encode()
                db.execute('INSERT INTO entries VALUES(?,?,?,?,?,?)',
                    (key, 'header', value, hashlib.sha256(value).hexdigest(), size, touched))
        self.assertEqual(check_totals()['entries'], 2)
        self.assertEqual(self.request('GET', '/api/fits-preview/M%2031_sub/sub.fit', raw=True)[0], 200)
        self.assertLessEqual(check_totals()['bytes'], 2147483648)
        with sqlite3.connect(db_path) as db:
            self.assertIsNone(db.execute("SELECT key FROM entries WHERE key='old'").fetchone())
            self.assertIsNotNone(db.execute("SELECT key FROM entries WHERE key='recent'").fetchone())
            db.execute("UPDATE entries SET size=123 WHERE key='recent'")
        check_totals()
        self.stop_server()
        with sqlite3.connect(db_path) as db:
            # Corrupt an existing preview to force a same-key replacement.
            db.execute("UPDATE entries SET value=x'00',checksum='bad' WHERE kind='png'")
        self.start_server()
        self.assertEqual(self.request('GET', '/api/fits-preview/M%2031_sub/sub.fit', raw=True)[0], 200)
        check_totals()
        with sqlite3.connect(db_path) as db:
            db.execute('DELETE FROM entries')
        self.assertEqual(check_totals()['bytes'], 0)
        self.post('/api/fits-cache/clear')
        self.wait_job('/api/fits-cache')
        self.assertEqual(check_totals()['entries'], 0)

    def test_summary_and_sub_details_keep_current_ratings_without_cross_target_items(self):
        other = self.library/'M 42_sub'; other.mkdir()
        write_fits(other/'other.fit', [0, 1, 2, 3])
        first = self.request('GET', '/api/library')[1]
        self.assertEqual(first['stats']['subRaw'], 2)
        self.post('/api/rating', {'id':'M 31_sub/sub.fit', 'scope':str(self.library), 'rating':5})
        detail = self.request('GET', '/api/sub-collection?target=M%2031')[1]['collection']
        self.assertEqual(detail['target'], 'M 31')
        items = [i for group in detail['groups'] for i in group['items']]
        self.assertEqual([(i['id'], i['rating']) for i in items], [('M 31_sub/sub.fit', 5)])
        self.assertEqual(self.request('GET', '/api/sub-collection?target=missing')[0], 404)
        again = self.request('GET', '/api/library')[1]
        self.assertTrue(again['scan']['cached'])
        self.assertEqual(again['subCollections'], first['subCollections'])
        self.assertTrue(all('items' not in g for c in again['subCollections'].values() for g in c['groups']))

    def test_changed_source_and_clear_invalidate_preview_etags(self):
        url = '/api/fits-preview/M%2031_sub/sub.fit'
        _, first, headers = self.request('GET', url, raw=True)
        write_fits(self.library/'M 31_sub/sub.fit', [.5, .02, .03, .01], bitpix=-32)
        status, second, fresh = self.request('GET', url, headers={'If-None-Match':headers['ETag']}, raw=True)
        self.assertEqual(status, 200)
        self.assertNotEqual(first, second)
        self.assertNotEqual(headers['ETag'], fresh['ETag'])
        self.post('/api/fits-cache/clear'); self.wait_job('/api/fits-cache')
        self.assertEqual(self.request('GET', url, headers={'If-None-Match':fresh['ETag']}, raw=True)[0], 200)

    def test_import_includes_nested_sub_and_preserves_sources(self):
        source = self.seestar/'2026/M 42_SUB/frame.fit'; source.parent.mkdir(parents=True)
        source.write_bytes(self.fit.read_bytes())
        self.post('/api/settings', {'seestarSource':str(self.seestar)})
        self.post('/api/seestar/import')
        result = self.wait_job('/api/seestar/import')
        self.assertEqual(result['imported'], 1, result)
        self.assertEqual((self.library/source.relative_to(self.seestar)).read_bytes(), source.read_bytes())
        self.post('/api/settings', {'seestarSource':str(self.library)})
        self.assertEqual(self.request('POST', '/api/seestar/import', {}, native=True)[0], 400)

    def test_edited_directory_is_independent_of_offline_original_library(self):
        other = self.edited/'M 42'; other.mkdir()
        (other/'final.png').write_bytes(b'finished-image')
        (self.edited/'SH2-103.jpg').write_bytes(b'loose-finished-image')
        self.post('/api/settings', {'libraryPath':str(self.root/'offline'),
                                   'editedSource':str(self.edited), 'galleryMode':'edited'})
        self.wait_job('/api/fits-cache')
        library = self.request('GET', '/api/library')[1]
        self.assertFalse(library['sourceStatus']['accessible'])
        self.assertTrue(library['editedStatus']['accessible'])
        self.assertEqual(library['items'], [])
        self.assertEqual(library['stats']['editedPhotos'], 3)
        self.assertEqual({item['category'] for item in library['editedItems']}, {'M 31', 'M 42', 'SH2-103'})
        self.assertEqual(set(library['editedByTarget']), {'M 31', 'M 42', 'SH2-103'})
        self.assertEqual(self.request('GET', '/edited-media/M%2042/final.png', raw=True)[1], b'finished-image')
        self.stop_server(); self.start_server()
        reloaded = self.request('GET', '/api/library')[1]
        self.assertEqual(reloaded['stats']['editedPhotos'], 3)
        self.assertEqual(reloaded['settings']['galleryMode'], 'edited')

    def test_edited_unmatched_targets_keep_separate_folder_names(self):
        other = self.edited/'M 42 - 猎户座'; other.mkdir()
        (other/'final.jpg').write_bytes(b'finished-image')
        self.post('/api/settings', {'editedSource':str(self.edited)})
        library = self.request('GET', '/api/library')[1]
        self.assertEqual({item['category'] for item in library['editedItems']}, {'M 31', 'M 42'})
        self.post('/api/settings', {'mergeTargets':False})
        library = self.request('GET', '/api/library')[1]
        self.assertIn('M 42 - 猎户座', {item['category'] for item in library['editedItems']})

    def test_cleanup_exact_target_and_merge_context(self):
        other = self.library/'M 310'; other.mkdir(); (other/'keep.jpg').write_bytes(b'keep')
        context = {'target':'M 31', 'source':str(self.library), 'mergeTargets':True}
        self.post('/api/cleanup-target/preview', context)
        job = self.wait_job('/api/cleanup-target')
        self.assertEqual(job['count'], 3)
        self.post('/api/settings', {'mergeTargets':False})
        self.assertEqual(self.request('POST', '/api/cleanup-target/execute', {**context, 'token':job['token']}, native=True)[0], 400)
        self.post('/api/cleanup-target/preview', {**context, 'mergeTargets':False})
        job = self.wait_job('/api/cleanup-target')
        self.assertEqual((job['masters'], job['subs']), (0, 1))
        self.assertTrue(self.fit.exists())
        self.assertTrue((other/'keep.jpg').exists())

    def test_seestar_cleanup_requires_equal_contents_and_valid_token(self):
        (self.seestar/'different.jpg').write_bytes(b'AAAA')
        (self.library/'different.jpg').write_bytes(b'BBBB')
        (self.seestar/'complete.jpg').write_bytes(b'CCCC')
        (self.library/'complete.jpg').write_bytes(b'CCCC')
        self.post('/api/settings', {'seestarSource':str(self.seestar)})
        self.post('/api/seestar/cleanup/preview')
        job = self.wait_job('/api/seestar/cleanup')
        self.assertEqual(job['count'], 1)
        self.assertEqual(self.request('POST', '/api/seestar/cleanup/execute', {'token':'wrong'}, native=True)[0], 400)
        self.assertTrue((self.seestar/'complete.jpg').exists())

if __name__=='__main__':
    unittest.main()
