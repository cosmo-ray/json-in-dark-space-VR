FROM --platform=linux/arm64 ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential pkg-config curl ca-certificates file \
      libglfw3-dev libegl-dev libgles-dev libopenxr-dev \
      xvfb libgl1-mesa-dri mesa-utils zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
