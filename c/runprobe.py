"""Run a tool from the bpclient-c-tagtest:dev image on the HMI Docker daemon.

Usage:
    py runprobe.py msgprobe --service 1 --path "01 02 20 01 24 01"
    py runprobe.py connidentity --slot 2

Streams demuxed container logs to stdout, returns the container exit code.

This file is throwaway — used during RE/empirical phases to exercise
diagnostic tools without rebuilding the image. Not part of the shipped SDK.
"""
import json, sys, time, urllib.request

HOST = 'http://10.0.0.166:2375'
IMAGE = 'bpclient-c-tagtest:dev'


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


def run(tool, args):
    cfg = {
        'Image': IMAGE,
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


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print('Usage: runprobe.py <tool> [args...]', file=sys.stderr)
        sys.exit(2)
    sys.exit(run(sys.argv[1], sys.argv[2:]))
