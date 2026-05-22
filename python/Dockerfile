# Python tagtest container.
#
# Build on the HMI:
#   py python/build_image.py
#
# Run (requires --ipc=host --pid=host -v /dev/shm:/dev/shm):
#   docker --host=tcp://10.0.0.166:2375 run --rm \
#     --ipc=host --pid=host -v /dev/shm:/dev/shm \
#     --platform linux/arm64 \
#     bpclient-python-tagtest:dev
#
# Or via the shared runner:
#   py runprobe.py --image bpclient-python-tagtest:dev tagtest
#
# SPDX-License-Identifier: MIT

FROM debian:bookworm-slim
ENV DEBIAN_FRONTEND=noninteractive \
    PYTHONUNBUFFERED=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    PIP_NO_CACHE_DIR=1

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        python3 python3-pip python3-venv ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY pyproject.toml ./
COPY src ./src
COPY examples ./examples
COPY tests ./tests

# Install the SDK + posix_ipc into the system site-packages.
# --break-system-packages bypasses PEP 668 for our throwaway image
# (no other Python tooling on this container to conflict with).
RUN python3 -m pip install --break-system-packages . posix_ipc>=1.1.1 pytest

# Drop each CLI example into /usr/local/bin/<name> (no .py suffix)
# so runprobe.py's hard-coded /usr/local/bin/<tool> entrypoint
# matches the C+Go images.
RUN set -eux; \
    for f in examples/*.py; do \
        name="$(basename "$f" .py)"; \
        install -m 0755 "$f" "/usr/local/bin/$name"; \
    done

# Default ENTRYPOINT is tagtest; override at run time with
# --entrypoint /usr/local/bin/<tool> for the diagnostic CLIs.
ENTRYPOINT ["/usr/local/bin/tagtest"]
