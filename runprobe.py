"""Run a tool from a bpclient-<lang>-tagtest:dev image on the HMI Docker daemon.

Usage:
    py runprobe.py [--image IMG] <tool> [args...]

Examples:
    py runprobe.py msgprobe --service 1 --path "01 02 20 01 24 01"
    py runprobe.py --image bpclient-go-tagtest:dev tagtest
    py runprobe.py --image bpclient-python-tagtest:dev tagtest

--image defaults to bpclient-c-tagtest:dev (the original C image). Each
language's build_image.py tags its own image; pass --image to target it.

Streams demuxed container logs to stdout, returns the container exit code.
Throwaway runner for RE/validation phases — not part of the shipped SDK.
"""
import json, sys, urllib.request

HOST = 'http://10.0.0.166:2375'
DEFAULT_IMAGE = 'bpclient-c-tagtest:dev'


def docker(method, path, body=None, headers=None, parse_json=True):
    url = HOST + path
    data = None
    h = {'Content-Type': 'application/json'} if body is not None else {}
    if headers:
        h.update(headers)
    if body is not None:
        data = json.dumps(body).encode() if isinstance(body, (dict, list)) else body
    req = urllib.request.Request(url, data=data, method=method, headers=h)
    with urllib.request.urlopen(req, timeout=60) as r:
        raw = r.read()
        if parse_json and raw:
            return json.loads(raw)
        return raw


def demux(buf):
    """Docker multiplexed stream: 8-byte header per chunk (stream, 0, 0, 0, size BE u32)."""
    out = []
    i = 0
    while i + 8 <= len(buf):
        sz = int.from_bytes(buf[i + 4:i + 8], 'big')
        i += 8
        out.append(buf[i:i + sz].decode('utf-8', 'replace'))
        i += sz
    return ''.join(out)


def run(image, tool, args):
    cfg = {
        'Image': image,
        'Entrypoint': [f'/usr/local/bin/{tool}'],
        'Cmd': list(args),
        'HostConfig': {
            'IpcMode': 'host',
            'PidMode': 'host',
            'Binds': ['/dev/shm:/dev/shm'],
            'AutoRemove': False,
        },
    }
    c = docker('POST', '/containers/create', cfg)
    cid = c['Id']
    try:
        docker('POST', f'/containers/{cid}/start', b'', parse_json=False)
        rc = docker('POST', f'/containers/{cid}/wait', b'', parse_json=True)
        logs = docker('GET', f'/containers/{cid}/logs?stdout=1&stderr=1', parse_json=False)
        sys.stdout.write(demux(logs))
        sys.stdout.flush()
        return rc.get('StatusCode', -1)
    finally:
        try:
            docker('DELETE', f'/containers/{cid}', parse_json=False)
        except Exception:
            pass


def parse_argv(argv):
    image = DEFAULT_IMAGE
    i = 1
    while i < len(argv) and argv[i].startswith('--'):
        if argv[i] == '--image':
            if i + 1 >= len(argv):
                print('--image requires a value', file=sys.stderr)
                sys.exit(2)
            image = argv[i + 1]
            i += 2
        elif argv[i].startswith('--image='):
            image = argv[i].split('=', 1)[1]
            i += 1
        else:
            break
    if i >= len(argv):
        print('Usage: runprobe.py [--image IMG] <tool> [args...]', file=sys.stderr)
        sys.exit(2)
    return image, argv[i], argv[i + 1:]


if __name__ == '__main__':
    image, tool, args = parse_argv(sys.argv)
    sys.exit(run(image, tool, args))
