#!/usr/bin/env bash
# tools/amiga-fetch-amissl-sdk.sh — fetch + unpack the AmiSSL v5 SDK
# for use by ports/amiga's TLS support (Phase 28).
#
# Designed to run **inside** the bebbo amiga-gcc container, where lha
# and curl are already available. Local container builds invoke this
# script via tools/amiga-build.sh (which bind-mounts the cache); the
# CI workflow invokes it directly in its container step.
#
# Idempotent: downloads + extracts only when the pinned-version
# directory is missing. Verifies SHA-256 on download. On success the
# absolute path to the extracted SDK root is written to stdout and to
# AMISSL_CACHE_DIR/latest (symlink), so callers don't have to know
# the version string.
#
# Usage:
#   tools/amiga-fetch-amissl-sdk.sh
#
# Environment:
#   AMISSL_VERSION       override the pinned version (default 5.27)
#   AMISSL_CACHE_DIR     cache root (default $HOME/.cache/amissl-sdk)
#
# Exit codes: 0 ok, 1 download/verify/extract failure, 2 usage.

set -euo pipefail

AMISSL_VERSION="${AMISSL_VERSION:-5.27}"
AMISSL_CACHE_DIR="${AMISSL_CACHE_DIR:-$HOME/.cache/amissl-sdk}"

# Pinned SHA-256 for the SDK archive. If AMISSL_VERSION is changed,
# the corresponding checksum must also be supplied via AMISSL_SHA256
# (the script refuses to silently fetch an unpinned version).
case "$AMISSL_VERSION" in
    5.27) PINNED_SHA256="5003bef8c5930354d16b0ce7196d71b2811891c42fad38a9238c5ce4098ad42a" ;;
    *)    PINNED_SHA256="${AMISSL_SHA256:-}" ;;
esac

if [ -z "$PINNED_SHA256" ]; then
    echo "amiga-fetch-amissl-sdk.sh: AMISSL_VERSION=$AMISSL_VERSION has no" >&2
    echo "  pinned SHA-256. Set AMISSL_SHA256 to the expected hash to proceed." >&2
    exit 2
fi

archive_name="AmiSSL-${AMISSL_VERSION}-SDK.lha"
archive_url="https://github.com/jens-maus/amissl/releases/download/${AMISSL_VERSION}/${archive_name}"
version_dir="$AMISSL_CACHE_DIR/amissl-${AMISSL_VERSION}"
sdk_root="$version_dir/sdk"
stamp="$version_dir/.ok"
latest_link="$AMISSL_CACHE_DIR/latest"

# Already extracted — refresh the latest symlink and bail out fast.
if [ -f "$stamp" ] && [ -d "$sdk_root/AmiSSL/Developer" ]; then
    ln -sfn "amissl-${AMISSL_VERSION}/sdk" "$latest_link"
    echo "$sdk_root/AmiSSL/Developer"
    exit 0
fi

mkdir -p "$version_dir"
archive_path="$version_dir/$archive_name"

# Tools we need. Refuse early with a clear message rather than fail
# halfway through extraction.
need() { command -v "$1" >/dev/null 2>&1 || { echo "amiga-fetch-amissl-sdk.sh: missing tool '$1'" >&2; exit 1; }; }
need curl
need lha
if command -v sha256sum >/dev/null 2>&1; then
    sha_cmd="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
    sha_cmd="shasum -a 256"
else
    echo "amiga-fetch-amissl-sdk.sh: need sha256sum or shasum on PATH" >&2
    exit 1
fi

echo "amiga-fetch-amissl-sdk.sh: fetching $archive_name" >&2
curl -fSL --retry 3 -o "$archive_path" "$archive_url"

got=$($sha_cmd "$archive_path" | awk '{print $1}')
if [ "$got" != "$PINNED_SHA256" ]; then
    echo "amiga-fetch-amissl-sdk.sh: checksum mismatch for $archive_name" >&2
    echo "  expected $PINNED_SHA256" >&2
    echo "  got      $got" >&2
    rm -f "$archive_path"
    exit 1
fi

# AmiSSL's SDK .lha unpacks into AmiSSL/Developer/{include,lib,...}.
# Headers live under .../include (with openssl/, proto/, etc.); the
# OS3 m68k stubs live under .../lib/AmigaOS3/libamisslstubs.a.
mkdir -p "$sdk_root"
(cd "$sdk_root" && lha xq "$archive_path")

dev_root="$sdk_root/AmiSSL/Developer"
if [ ! -d "$dev_root/include/openssl" ] \
   || [ ! -f "$dev_root/lib/AmigaOS3/libamisslstubs.a" ]; then
    echo "amiga-fetch-amissl-sdk.sh: unexpected SDK layout under $sdk_root" >&2
    find "$sdk_root" -maxdepth 4 -type d >&2
    exit 1
fi

# Stamp on success so subsequent runs short-circuit.
touch "$stamp"
ln -sfn "amissl-${AMISSL_VERSION}/sdk" "$latest_link"

# Callers add -I<root>/include and -L<root>/lib/AmigaOS3 to their
# build, then -lamisslstubs. Print the Developer dir so they can
# compute both relative paths.
echo "$dev_root"
