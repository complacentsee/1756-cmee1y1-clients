# Multi-stage build for the Go tagtest container.
#
# Build on the HMI (or any aarch64 machine with Docker):
#   docker --host=tcp://10.0.0.166:2375 build -t bpclient-go-tagtest:dev -f Dockerfile .
# Or:
#   py go/build_image.py
#
# Run (requires --ipc=host --pid=host -v /dev/shm:/dev/shm):
#   docker --host=tcp://10.0.0.166:2375 run --rm \
#     --ipc=host --pid=host -v /dev/shm:/dev/shm \
#     --platform linux/arm64 \
#     bpclient-go-tagtest:dev
#
# Or via the shared runner:
#   py runprobe.py --image bpclient-go-tagtest:dev tagtest
#
# SPDX-License-Identifier: MIT

FROM golang:1.22-bookworm AS build
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential ca-certificates && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY go.mod ./
COPY go.sum* ./
RUN go mod download
COPY . .
RUN mkdir -p /out/bin && \
    CGO_ENABLED=1 go build -trimpath -ldflags '-s -w' -o /out/bin/tagtest      ./cmd/tagtest      && \
    CGO_ENABLED=1 go build -trimpath -ldflags '-s -w' -o /out/bin/msgprobe     ./cmd/msgprobe     && \
    CGO_ENABLED=1 go build -trimpath -ldflags '-s -w' -o /out/bin/identity     ./cmd/identity     && \
    CGO_ENABLED=1 go build -trimpath -ldflags '-s -w' -o /out/bin/connidentity ./cmd/connidentity && \
    CGO_ENABLED=1 go build -trimpath -ldflags '-s -w' -o /out/bin/pathprobe    ./cmd/pathprobe    && \
    CGO_ENABLED=1 go build -trimpath -ldflags '-s -w' -o /out/bin/actnodes     ./cmd/actnodes     && \
    CGO_ENABLED=1 go build -trimpath -ldflags '-s -w' -o /out/bin/modutil      ./cmd/modutil

# Runtime: distroless/cc carries glibc + dynamic loader (needed
# because the binaries link against libpthread/librt via cgo for
# POSIX named-sem support).  glibc version matches the build stage
# (bookworm / glibc 2.36).
FROM gcr.io/distroless/cc-debian12
COPY --from=build /out/bin/ /usr/local/bin/
# Default ENTRYPOINT is tagtest (canonical smoke test).  Override
# at run time to pick a different diagnostic:
#   --entrypoint /usr/local/bin/msgprobe
ENTRYPOINT ["/usr/local/bin/tagtest"]
