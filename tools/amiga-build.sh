#!/usr/bin/env bash
# tools/amiga-build.sh — build the Amiga port inside the same Docker
# container CI uses, so local and CI binaries are identical.
#
# Usage:
#   tools/amiga-build.sh                       # build all four variants
#   tools/amiga-build.sh standard              # build one
#   tools/amiga-build.sh standard 68040        # build several
#   tools/amiga-build.sh clean                 # clean all variant build dirs
#
# Environment:
#   AMIGA_DOCKER_IMAGE   override the image tag (default: stefanreinauer/amiga-gcc:latest)
#
# Output goes to the standard ports/amiga/build-<variant>/ paths, so
# tools/amiga-vamos-run.sh and friends find the binaries unchanged.
#
# Files created inside the container are owned by the host user (via
# --user), so no root-owned artifacts end up in the working tree.
#
# Note: mpy-cross/build/ produced by this script holds Linux ELF binaries;
# a native macOS bebbo build would put Mach-O there. Don't interleave the
# two without running 'tools/amiga-build.sh clean' first.

set -euo pipefail

IMAGE="${AMIGA_DOCKER_IMAGE:-stefanreinauer/amiga-gcc:latest}"
REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
ALL_VARIANTS=(standard minimal 68020fpu 68040)

# Platform hint only matters on non-amd64 hosts (e.g. Apple Silicon);
# harmless on linux/amd64.
PLATFORM_FLAG="--platform linux/amd64"

run_in_container() {
    docker run --rm $PLATFORM_FLAG \
        --user "$(id -u):$(id -g)" \
        -e HOME=/tmp \
        -v "$REPO_DIR":/workspace -w /workspace \
        "$IMAGE" \
        bash -c "$1"
}

if [ $# -ge 1 ] && [ "$1" = "clean" ]; then
    cmds='git config --global --add safe.directory /workspace; '
    cmds+='make -C mpy-cross clean; '
    for v in "${ALL_VARIANTS[@]}"; do
        cmds+="make -C ports/amiga VARIANT=$v clean; "
    done
    run_in_container "$cmds"
    exit 0
fi

if [ $# -eq 0 ]; then
    variants=("${ALL_VARIANTS[@]}")
else
    variants=("$@")
fi

# Validate variant names before launching the container.
for v in "${variants[@]}"; do
    case " ${ALL_VARIANTS[*]} " in
        *" $v "*) ;;
        *)
            echo "amiga-build.sh: unknown variant '$v' (known: ${ALL_VARIANTS[*]})" >&2
            exit 2
            ;;
    esac
done

build_script='
    set -e
    git config --global --add safe.directory /workspace
    make -C ports/amiga submodules
    make -C mpy-cross -j$(nproc)
'
for v in "${variants[@]}"; do
    build_script+="
    echo '=== Building $v ==='
    make -C ports/amiga VARIANT=$v -j\$(nproc)
"
done

run_in_container "$build_script"
