"""Launch only the synthetic offline GUI fixture, with a bounded lifetime and no visible console."""
import json
import os
from pathlib import Path
import subprocess
import time
import tempfile
import sys

build_bin = Path(os.environ.get('NOCTIS_BUILD_BIN', 'D:/devtools/leafyrino-build/build/bin'))
qt = Path(os.environ.get('QTDIR', 'D:/devtools/leafyrino-build/Qt/6.9.3/msvc2022_64'))
root = Path(tempfile.mkdtemp(prefix='noctis-localization-'))
print(root)
env = dict(os.environ, QT_QPA_PLATFORM='offscreen', QT_PLUGIN_PATH=str(qt / 'plugins'))
env['PATH'] = os.pathsep.join((str(qt / 'bin'), str(build_bin), env.get('PATH', '')))
for key in ('TEMP','TMP','APPDATA','LOCALAPPDATA'):
    path=root/('isolated-'+key.lower());path.mkdir(exist_ok=True);env[key]=str(path)
begin=time.monotonic()
try:
    result=subprocess.run([build_bin / 'Leafyrino-localization-tests.exe', str(root), *sys.argv[1:]],
                          env=env,capture_output=True,timeout=60,creationflags=subprocess.CREATE_NO_WINDOW)
    (root/'preview-stdout.log').write_bytes(result.stdout)
    (root/'preview-stderr.log').write_bytes(result.stderr)
    print(json.dumps(dict(returncode=result.returncode,seconds=round(time.monotonic()-begin,3),
                          report_exists=(root/'ui-results.json').exists())))
    print(result.stderr.decode('utf-8','replace')[-1500:])
    if result.returncode:raise SystemExit(result.returncode)
except subprocess.TimeoutExpired:
    raise SystemExit('Synthetic fixture timed out; only this test process was stopped.')
