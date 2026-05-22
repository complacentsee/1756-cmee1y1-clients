"""Build the Go tagtest Docker image on the HMI.  Normalizes tar
ownership to 0:0 to avoid Windows UID metadata tripping rootless
Docker's lchown.

Outputs the bpclient-go-tagtest:dev image.  After this completes,
run e.g.

    py runprobe.py --image bpclient-go-tagtest:dev tagtest
"""
import io, os, tarfile, urllib.request, sys

here = os.path.dirname(os.path.abspath(__file__))
buf = io.BytesIO()
with tarfile.open(fileobj=buf, mode='w:gz') as tf:
    for root, dirs, files in os.walk(here):
        # skip build/cache/version-control artifacts
        dirs[:] = [d for d in dirs if d not in ('build', '.git', '__pycache__', 'bin', 'out')]
        for name in files:
            if name == 'build_image.py':
                continue
            full = os.path.join(root, name)
            arc = os.path.relpath(full, here).replace(os.sep, '/')
            ti = tf.gettarinfo(full, arc)
            ti.uid = 0
            ti.gid = 0
            ti.uname = 'root'
            ti.gname = 'root'
            with open(full, 'rb') as f:
                tf.addfile(ti, f)

buf.seek(0)
print(f'tar built: {len(buf.getvalue())} bytes', flush=True)

url = 'http://10.0.0.166:2375/build?t=bpclient-go-tagtest:dev&dockerfile=Dockerfile&rm=1'
req = urllib.request.Request(url, data=buf.getvalue(), method='POST',
                              headers={'Content-Type': 'application/x-tar'})
with urllib.request.urlopen(req, timeout=600) as r:
    for line in r:
        # streamed JSON lines
        try:
            import json
            d = json.loads(line)
            if 'stream' in d:
                print(d['stream'], end='', flush=True)
            elif 'errorDetail' in d:
                print('ERROR:', d['errorDetail'], flush=True)
                sys.exit(1)
            elif 'aux' in d:
                print(d['aux'], flush=True)
        except Exception:
            print(line.decode(errors='replace'), end='', flush=True)
