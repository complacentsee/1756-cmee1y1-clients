# Container plumbing

Every container that uses one of these SDKs needs the SAME runtime
configuration. This page is the canonical reference; the per-language
READMEs link back here.

## The required flags

```
--ipc=host                          # share host's POSIX IPC namespace (sem + shm)
--pid=host                          # bpServer uses kill(pid, 0) liveness checks
-v /dev/shm:/dev/shm                # redundant w/ --ipc=host but harmless and clearer
--platform linux/arm64              # cm1756 is aarch64
```

### Why each one

#### `--ipc=host`

Without this, the container has its own POSIX IPC namespace and
`sem_open("/bpShm")` opens a fresh semaphore that bpServer never sees.

With `--ipc=host`, the named semaphores at `/dev/shm/sem.bp*` are
resolvable from inside the container and refer to the same kernel
objects the host's bpServer is using.

#### `--pid=host`

bpServer does `kill(client_pid, 0)` periodically to verify clients
are alive — if it can't see the client's PID (because the container
has its own PID namespace), it treats the slot as orphaned and
recycles it, which crashes your in-flight request.

`--pid=host` puts the container in the host PID namespace so client
PIDs are visible.

> Note: `--pid=host` also means the container can SEE host processes
> in `/proc` and signal them. Don't run untrusted code with this
> flag. Our SDK trusts the host (it's a control system) so it's
> appropriate here.

#### `-v /dev/shm:/dev/shm`

Strictly redundant with `--ipc=host` on most Docker installations
(the IPC namespace already provides shared `/dev/shm`). Some
hardened configurations or non-Docker runtimes (`podman --userns=host`)
need both. We always include both because it's cheap and clearer.

#### `--platform linux/arm64`

The cm1756 HMI is on a NXP i.MX8MP (aarch64). When you build images
from a developer x86_64 machine and push to the HMI, Docker will
multi-arch select the wrong image without this flag.

If you're building the image ON the HMI itself, this flag has no
effect (Docker uses native arch automatically).

## What you DON'T need

| Flag | Why we DON'T need it |
|---|---|
| `--privileged` | Default seccomp + uid 0 in container is enough |
| `--cap-add ANY` | No kernel-level operations |
| `-v /usr/lib:...` (vendor libs) | Our SDK speaks the wire protocol natively; doesn't link the vendor library |
| `--device /dev/ocx_cbregs` | That's the kernel-mailbox device for direct ASIC access; this SDK uses the userland IPC, not the kernel mailbox |
| `--device /dev/ocx_shram` | Same as above |
| `--device /dev/caam*` | CAAM is firmware-update territory, not tag I/O |
| Network capabilities | None — this is all local IPC |

## docker-compose snippets

### Python

```yaml
services:
  bpclient:
    build: ./python                  # or image: ghcr.io/...
    ipc: host
    pid: host
    volumes:
      - /dev/shm:/dev/shm
    platform: linux/arm64
    environment:
      - BP_PATH=P:1,S:2              # CIP route to your PLC
    command: python -m bpclient.examples.tagtest --tag OCX_TEST
```

### Go

```yaml
services:
  bpclient:
    build: ./go
    ipc: host
    pid: host
    volumes:
      - /dev/shm:/dev/shm
    platform: linux/arm64
    environment:
      - BP_PATH=P:1,S:2
    command: /usr/local/bin/tagtest -path "P:1,S:2" -tag OCX_TEST
```

### C

```yaml
services:
  bpclient:
    build: ./c
    ipc: host
    pid: host
    volumes:
      - /dev/shm:/dev/shm
    platform: linux/arm64
    command: /usr/local/bin/tagtest --path "P:1,S:2" --tag OCX_TEST
```

All three are byte-for-byte identical in their `ipc:` / `pid:` /
`volumes:` / `platform:` sections. That's intentional.

## `docker run` equivalents

```sh
docker run --rm -it \
    --ipc=host --pid=host \
    -v /dev/shm:/dev/shm \
    --platform linux/arm64 \
    ghcr.io/complacentsee/1756-cmee1y1-clients-python:latest \
    python -m bpclient.examples.tagtest
```

## Troubleshooting

### `open /dev/shm/bpShmem: ENOENT`

bpServer isn't running on the host. Check `systemctl status bpServer`
on the HMI.

### `sem_open(/bpShm): EACCES`

The kernel won't let you open the semaphore. Verify `--ipc=host`
is set (not just `-v /dev/shm:/dev/shm`).

### `OpenSession() never returns`

bpServer received the request but its reply went somewhere we can't
see. Check the container's PID namespace — `--pid=host` is missing.

### `EACCES` opening any `/dev/shm/sem.bp*`

The host file is mode 660 owned by `bpapi:bpapi` (uid 1007). Run
the container as root (the default) — uid 0 in the container, with
`--ipc=host`, accesses those files as the host's uid 0 if userns
isn't remapped, or as the remap target if it is.

If your Docker daemon has `userns-remap` enabled, container root
becomes a non-1007 host user, and access fails. Workarounds:

1. Disable `userns-remap` in `/etc/docker/daemon.json` (system-wide)
2. Run the container with `--userns=host` (per-container; only
   works if the daemon allows it)
3. Run the container as the host uid that matches bpServer:
   `--user 1007:1007` (in the container, you'll be uid 1007)

Option 3 is the cleanest for production. We don't enable it by
default in the example compose files because it requires the
container's binaries to be readable by uid 1007 — which the example
images set up by default, but custom builds may forget.

### `-103001 NO_FREE_SLOT` even though I only have one client

Look for stale clients: a previous container that exited without
cleaning up may have left its slot owned. Restart bpServer (which
will reset all slot ownership) or wait 5 minutes for the engine to
GC orphaned slots via its liveness check.

## Production deployment notes

- **Run as a sidecar.** Your application container and the
  bpclient container share host IPC. They don't need to share
  anything else.
- **Health checks.** The Python and Go examples expose a `--ping`
  flag that returns 0 if the IPC is healthy. Wire this to your
  container orchestrator's `healthcheck:` directive.
- **Logging.** This SDK logs to stderr only. No log file rotation
  in the SDK itself; your orchestrator's log driver handles that.
- **Restart policy.** `restart: unless-stopped` is appropriate.
  The SDK handles bpServer restarts gracefully — it'll fail the
  in-flight request, return `NOT_OPEN`, and reconnect on the next
  call.
