# Multi-stage build for the C tagtest container.
#
# Build on the HMI (or any aarch64 machine with Docker):
#   docker --host=tcp://10.0.0.166:2375 build -t bpclient-c-tagtest:dev -f Dockerfile .
#
# Run (requires --ipc=host --pid=host -v /dev/shm:/dev/shm):
#   docker --host=tcp://10.0.0.166:2375 run --rm \
#     --ipc=host --pid=host -v /dev/shm:/dev/shm \
#     --platform linux/arm64 \
#     bpclient-c-tagtest:dev
#
# SPDX-License-Identifier: MIT

FROM debian:bookworm-slim AS build
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential cmake ca-certificates && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --parallel && \
    cmake --install build --prefix /out


FROM debian:bookworm-slim
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        libc6 && \
    rm -rf /var/lib/apt/lists/*
COPY --from=build /out/lib/libbpclient.so* /usr/local/lib/
COPY --from=build /out/bin/tagtest /usr/local/bin/tagtest
RUN ldconfig
ENTRYPOINT ["/usr/local/bin/tagtest"]
