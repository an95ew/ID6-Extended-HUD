# Linux x86 host for the QNX SDP 6.5 toolchain (32-bit host tools), runs under emulation on Apple silicon.
FROM --platform=linux/amd64 debian:bookworm-slim
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      libc6:i386 libstdc++6:i386 zlib1g:i386 libncurses5:i386 libgcc-s1:i386 \
      libxext6:i386 libxrender1:i386 libxtst6:i386 libxi6:i386 \
      make python3 ca-certificates procps \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
