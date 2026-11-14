# The Vita build's machine for a computer that is not Linux (macOS, or Windows without WSL): VitaSDK's own image
# (Ubuntu 24.04, the SDK and its packages: zlib, math-neon, vitaShaRK, SceShaccCgExt, taihen), plus libclang's Python
# bindings, vitasdk/samples and vitaGL with this port's patch.
# Built and used by port/vita/tools/docker_build.sh; the checkout is mounted at /src, nothing of it is copied in.
ARG BASE=vitasdk/vitasdk:latest
FROM ${BASE}
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends python3-clang libclang-dev cmake \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 https://github.com/vitasdk/samples /opt/vitasdk-samples
COPY vitagl.patch build_vitagl.sh /opt/bt3-vitagl/
RUN sh /opt/bt3-vitagl/build_vitagl.sh /opt/vitagl && rm -rf /opt/vitagl
ENV VITA_SAMPLES=/opt/vitasdk-samples
WORKDIR /src
