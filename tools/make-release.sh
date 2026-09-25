#!/usr/bin/env bash
# Build a GitHub release of Kasumi.
#
#   MAKEROM=/path/to/makerom BANNERTOOL=/path/to/bannertool bash tools/make-release.sh
#
# Produces dist/v<VERSION>/ with Kasumi.cia, Kasumi.3dsx and SHA256SUMS (the
# in-app updater refuses a release without SHA256SUMS), then prints the
# `gh release create` command to publish it. Set the version in the Makefile
# (VERSION_MAJOR / MINOR / MICRO / SUFFIX) and write the release notes in
# docs/releases/v<VERSION>.md first: the updater shows those notes on the 3DS.
set -euo pipefail
cd "$(dirname "$0")/.."

: "${MAKEROM:?Set MAKEROM to the makerom executable}"
: "${BANNERTOOL:?Set BANNERTOOL to the bannertool executable}"

version=$(make -s version)
notes="docs/releases/v$version.md"
if [ ! -f "$notes" ]; then
    echo "Write the release notes first: $notes" >&2
    exit 1
fi

# A clean build so every object carries the new version.
make clean >/dev/null
make
make cia MAKEROM="$MAKEROM" BANNERTOOL="$BANNERTOOL"

out="dist/v$version"
rm -rf "$out"
mkdir -p "$out"
cp Kasumi.cia Kasumi.3dsx "$out/"
(cd "$out" && sha256sum Kasumi.cia Kasumi.3dsx > SHA256SUMS)

# QR code for FBI's Remote install, pointing at this release's CIA. It goes
# into the README, so commit docs/assets/install-qr.png with the release.
python="${PYTHON:-python3}"
if command -v "$python" >/dev/null 2>&1; then
    "$python" tools/make_qr.py \
        "https://github.com/p0mpurin/Kasumi/releases/download/v$version/Kasumi.cia" \
        docs/assets/install-qr.png
    cp docs/assets/install-qr.png "$out/install-qr.png"
else
    echo "Skipping the QR code: set PYTHON to a Python 3 interpreter" >&2
fi

prerelease=""
case "$version" in *-*) prerelease="--prerelease" ;; esac

echo
echo "Release files in $out:"
ls -l "$out"
echo
echo "Publish with:"
echo "  gh release create v$version $out/Kasumi.cia $out/Kasumi.3dsx $out/SHA256SUMS \\"
echo "    --title \"Kasumi $version\" --notes-file $notes $prerelease"
