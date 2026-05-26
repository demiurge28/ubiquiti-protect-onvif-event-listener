#!/usr/bin/env bash
# build-in-docker.sh — build onvif_recorder inside a Docker container
#
# Usage:
#   ./build-in-docker.sh              # build x86-64 and ARM64 binaries
#   ./build-in-docker.sh --x86        # x86-64 only
#   ./build-in-docker.sh --arm64      # ARM64 only
#   ./build-in-docker.sh --test       # build x86-64 + run tests
#   ./build-in-docker.sh --deb        # build ARM64 release + .deb package
#   ./build-in-docker.sh --rebuild    # force rebuild of the Docker image
#
# Output binaries are written to dist/ in the project root:
#   dist/onvif_recorder                          x86-64 binary
#   dist/onvif_recorder.arm64                    ARM64 binary (for Dream Machine / NVR)
#   dist/onvif-recorder_<version>_arm64.deb      Debian package (with --deb)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="onvif-recorder-builder:v3"  # bump tag when Dockerfile package list changes
CACHE_VOL="onvif-recorder-bazel-cache"

# File that records the git commit SHA of the last successful ARM64 Docker build.
# Lets the pre-push hook (and manual invocations) skip a rebuild when nothing has
# changed since the previous run.  Listed in .gitignore.
ARM64_BUILD_CACHE="$SCRIPT_DIR/.docker_arm64_cache"

BUILD_X86=true
BUILD_ARM64=true
RUN_TESTS=false
REBUILD_IMAGE=false
BUILD_DEB=false

for arg in "$@"; do
    case "$arg" in
        --x86)     BUILD_ARM64=false ;;
        --arm64)   BUILD_X86=false; BUILD_ARM64=true ;;
        --test)    BUILD_X86=true; BUILD_ARM64=false; RUN_TESTS=true ;;
        --deb)     BUILD_X86=false; BUILD_ARM64=true; BUILD_DEB=true ;;
        --rebuild) REBUILD_IMAGE=true ;;
        *) echo "Unknown argument: $arg" >&2; exit 1 ;;
    esac
done

# Current HEAD commit SHA -- used to detect whether anything changed since the
# last successful ARM64 Docker build.
CURRENT_SHA=$(git -C "$SCRIPT_DIR" rev-parse HEAD 2>/dev/null || echo "")

# --rebuild clears the ARM64 build cache so the next run always rebuilds.
if $REBUILD_IMAGE; then
    rm -f "$ARM64_BUILD_CACHE"
fi

# ---------------------------------------------------------------------------
# 1. Build the Docker image (once, or when --rebuild is passed)
# ---------------------------------------------------------------------------
if $REBUILD_IMAGE || ! docker image inspect "$IMAGE" &>/dev/null; then
    echo "==> Building Docker image $IMAGE ..."
    docker build -t "$IMAGE" - <<'DOCKERFILE'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    clang \
    cmake \
    ninja-build \
    perl \
    lld \
    nasm \
    python3-pip \
    curl \
    git \
    dpkg-dev \
    fakeroot \
    xz-utils \
    binutils-aarch64-linux-gnu \
    autoconf \
    automake \
    libtool \
    pkg-config \
    m4 \
  && pip3 install --break-system-packages cpplint \
  && rm -rf /var/lib/apt/lists/*

# Install Bazelisk as /usr/local/bin/bazel; it downloads the correct Bazel
# version from .bazelversion on first use.  Select the binary for the
# container's native architecture (amd64 or arm64).
RUN arch=$(dpkg --print-architecture) \
  && curl -fsSL -o /usr/local/bin/bazel \
    "https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-${arch}" \
  && chmod +x /usr/local/bin/bazel
DOCKERFILE
fi

# ---------------------------------------------------------------------------
# 2. Ensure the Bazel cache volume exists (persists across runs)
#    Sub-directories inside the volume provide per-config isolation:
#      /root/.cache/bazel/x86       — x86 build cache
#      /root/.cache/bazel/arm64     — ARM64 cross-compile cache
#      /root/.cache/bazel/arm64_release — ARM64 PGO+LTO release cache
# ---------------------------------------------------------------------------
docker volume create "$CACHE_VOL" > /dev/null

mkdir -p "$SCRIPT_DIR/dist"

# Run a Docker container for a single build step.
# $1 = Bazel config name (used as --output_base sub-directory)
# $2 = shell command to run inside the container
run() {
    local config="$1"
    local cmd="$2"
    docker run --rm \
        -v "$SCRIPT_DIR:/build" \
        -v "$CACHE_VOL:/root/.cache/bazel" \
        -w /build \
        "$IMAGE" \
        bash -c "bazel --output_base=/root/.cache/bazel/$config $cmd"
}

# ---------------------------------------------------------------------------
# 3. Build and copy outputs to dist/
# ---------------------------------------------------------------------------
if $BUILD_X86; then
    # Skip the x86-64 binary build when running on ARM64 (tests run natively
    # with --config=arm64 instead; the x86 config is for x86-64 Linux hosts).
    HOST_ARCH=$(uname -m)
    if [ "$HOST_ARCH" = "aarch64" ] || [ "$HOST_ARCH" = "arm64" ]; then
        echo "==> Skipping x86-64 binary build (ARM64 host)"
    else
        echo "==> Building x86-64 binary ..."
        run "x86" "build --config=x86 //:onvif_recorder \
          && cp -f bazel-bin/onvif_recorder dist/onvif_recorder"
        echo "==> dist/onvif_recorder"
    fi
fi

if $RUN_TESTS; then
    echo "==> Running tests ..."
    # Select the test build config based on the container's native architecture.
    # On ARM64 hosts (Apple Silicon + colima), --config=arm64 activates the
    # arm64_sysroot cross-toolchain which provides the prebuilt system headers;
    # --config=x86 only works on x86-64 hosts where the host libraries are used.
    DOCKER_ARCH=$(uname -m)
    if [ "$DOCKER_ARCH" = "aarch64" ] || [ "$DOCKER_ARCH" = "arm64" ]; then
        echo "    (ARM64 host: using --config=arm64_native for tests)"
        run "arm64_native" "test --config=arm64_native //test:all"
    else
        run "x86" "test --config=x86 //test:all"
    fi
fi

if $BUILD_ARM64; then
    if $BUILD_DEB; then
        echo "==> Building ARM64 release binary (PGO + ThinLTO) ..."
        run "arm64_release" "build --config=arm64_release //:onvif_recorder \
          && cp -f bazel-bin/onvif_recorder dist/onvif_recorder.arm64"
        echo "==> dist/onvif_recorder.arm64"
    else
        CACHED_SHA=$(cat "$ARM64_BUILD_CACHE" 2>/dev/null || echo "")
        if [ -n "$CURRENT_SHA" ] && [ "$CURRENT_SHA" = "$CACHED_SHA" ]; then
            echo "==> ARM64 Docker build skipped (already built at $(git -C "$SCRIPT_DIR" log -1 --oneline 2>/dev/null || echo "$CURRENT_SHA"))"
        else
            echo "==> Building ARM64 binary ..."
            run "arm64" "build --config=arm64 //:onvif_recorder \
              && cp -f bazel-bin/onvif_recorder dist/onvif_recorder.arm64"
            echo "==> dist/onvif_recorder.arm64"
            echo "$CURRENT_SHA" > "$ARM64_BUILD_CACHE"
        fi
    fi
fi

if $BUILD_DEB; then
    # Compute version on the host (git inside docker trips "dubious ownership"
    # on bind-mounted repos; strip the leading "v").
    DEB_VERSION=$(git -C "$SCRIPT_DIR" describe --tags --dirty 2>/dev/null \
                  | sed 's/^v//' || echo "0.0.0-dev")
    echo "==> Assembling .deb (version=$DEB_VERSION) ..."
    docker run --rm \
        -v "$SCRIPT_DIR:/build" \
        -w /build \
        "$IMAGE" \
        bash -c "scripts/build-deb.sh --arch=arm64 --binary=dist/onvif_recorder.arm64 --version=$DEB_VERSION"
fi
