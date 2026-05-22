# C client library

Outbound tag I/O against the 1756-CMEE1Y1 backplane via the userland
wrapper IPC. Pure C11, depends only on `libpthread` + `librt` (both
in glibc).

> One of three implementations of the same wire protocol — the
> [Go SDK](../go/) and [Python SDK](../python/) live alongside this
> one. All three pass `tagtest` + `msgprobe` byte-identically; see
> [`runprobe.py`](../runprobe.py) for the shared cross-language
> runner.

## Build

```sh
# from this directory
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Produces:
- `build/libbpclient.so.0.7.0` — shared library
- `build/libbpclient.a` — static library
- `build/tagtest` — the canonical smoke test executable
  (plus the other diagnostic binaries listed in
  [`CMakeLists.txt`](CMakeLists.txt): `msgprobe`, `identity`,
  `connidentity`, `conntest`, `pathprobe`, `actnodes`, `modutil`, etc.)

System install:

```sh
sudo cmake --install build --prefix /usr/local
# headers:    /usr/local/include/bpclient.h
# libraries:  /usr/local/lib/libbpclient.so*
# tagtest:    /usr/local/bin/tagtest
```

## Use in your own program

```c
#include <stdio.h>
#include <bpclient.h>

int main(void) {
    bp_client_t *c;
    if (bp_client_open(&c) != BP_OK) return 1;

    uint32_t session;
    bp_client_open_session(c, &session);

    bp_tagdb_t *db;
    bp_tagdb_open(c, "P:1,S:2", &db);
    bp_tagdb_build(db, NULL);

    int32_t value;
    if (bp_tagdb_read_dint(db, "OCX_TEST", &value) == BP_OK) {
        printf("OCX_TEST = %d\n", value);
    }

    bp_tagdb_write_dint(db, "OCX_TEST", 42);

    bp_tagdb_close(db);
    bp_client_close(c);
    return 0;
}
```

Compile:

```sh
cc -o myapp myapp.c -lbpclient -lpthread -lrt
```

## Container

```sh
docker build -t bpclient-c-tagtest:dev -f Dockerfile .
docker run --rm \
    --ipc=host --pid=host \
    -v /dev/shm:/dev/shm \
    --platform linux/arm64 \
    bpclient-c-tagtest:dev \
    --path "P:1,S:2" --tag OCX_TEST
```

Or via compose:

```sh
docker compose up
```

The `--ipc=host`, `--pid=host`, `-v /dev/shm:/dev/shm` flags are
mandatory — see [../docs/container-plumbing.md](../docs/container-plumbing.md).

## Run options

```
tagtest [options]
  --path PATH       OldI CIP route (default: P:1,S:2)
  --tag NAME        tag to read/write/restore (default: OCX_TEST)
  --dump N          dump first N symbols after Build (default: 10)
  --no-write        skip the write/restore cycle (read-only)
  --help            this help
```

Exit codes: `0` PASS, `1` FAIL (data mismatch), `2` FATAL (IPC error).

## Layout

```
c/
├── include/bpclient.h       public C API
├── src/
│   ├── client.c              Open/Close + slot dispatch (bp_client_call)
│   ├── tagdb.c               CreateTagDbHandle / BuildTagDb / GetSymbolInfo
│   ├── access.c              AccessTagData + scalar R/W helpers
│   ├── message.c             OCXcip_MessageSend (UCMM CIP)
│   ├── conn.c                bp_client_txrx_* (LFO + MessageSend, v0.7.0+)
│   ├── identity.c            OCXcip_GetIdObject / GetDeviceIdObject
│   ├── module.c              modutil helpers (switch / display / LED)
│   ├── errors.c              bp_strerror()
│   └── proto.h               internal: layout constants
├── examples/tagtest.c
├── CMakeLists.txt
├── Dockerfile
└── docker-compose.yml
```

## Thread safety

`bp_client_t` is safe to use from multiple threads concurrently —
each `bp_client_call` holds a slot for the duration of one request,
and slot allocation is protected by both a cross-process POSIX
semaphore (`/bpShm`) and a process-local `pthread_mutex_t`.

## Error handling

All public functions return `int`. `BP_OK` (0) is success. Any
negative value is an error matching the `BP_ERR_*` constants in
`bpclient.h` — convert to a string with `bp_strerror(rc)`.

`bp_tagdb_access` and the convenience helpers set per-request
`result` fields with CIP General Status codes (0 = ok, 0x05 = path
unknown, 0x15 = too much data, etc.) — these are independent of the
function return value.
