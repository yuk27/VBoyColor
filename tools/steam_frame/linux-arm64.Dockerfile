# Build image for the native Linux ARM64 apps (Steam Frame / SteamOS aarch64).
# Ubuntu 22.04 keeps the binaries' glibc (2.35) and libstdc++ (GLIBCXX 3.4.30)
# needs below what the Frame's Steam Linux Runtime offers. Same dependencies
# as the CI's linux job, plus the toolchain a bare Ubuntu image lacks.
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ninja-build git ca-certificates curl file binutils \
        libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libvulkan1 \
    && rm -rf /var/lib/apt/lists/*
# Kitware's CMake (Ubuntu 22.04 ships 3.22, the project's bare minimum)
ARG CMAKE_VERSION=3.31.6
RUN curl -fsSL https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-aarch64.tar.gz \
    | tar xz -C /opt && ln -s /opt/cmake-${CMAKE_VERSION}-linux-aarch64/bin/* /usr/local/bin/
# (the repo is bind-mounted and owned by another uid)
RUN git config --global --add safe.directory '*'
WORKDIR /src
