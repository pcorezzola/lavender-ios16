#!/bin/bash
# Build the LAVENDER kernel on the physical Mac (192.168.0.5) instead of GitHub Actions.
#
#   tools/build-xnu-on-mac.sh [OUT_DIR]      (run from the repo root, on the PC; bash)
#
# Sends this checkout's committed xnu/patches (git archive: LF endings, see below) to the Mac, rebuilds the kernel there with
# Xcode 16.2 (the same dxb build.sh and pinned commit the workflow uses) and copies kernel.development.lavender back to OUT_DIR
# (default work/kernel-mac). Measured 2026-11-23: 3 min 27 s from scratch, against 7 min 28 s on the GitHub runner (plus queue).
#
# Things that went wrong the first time, and why this script does what it does:
#  * Patches must come from `git archive`, not from the working tree: with core.autocrlf some patch files are CRLF on Windows and
#    `git apply` on the Mac silently SKIPS them (0336+ were dropped, the kernel then failed to link: undefined LavenderGPUPoolEarly).
#  * build.sh installs the Kernel Debug Kit with `sudo installer`. There is no sudo without a terminal, so the KDK package is expanded
#    into ~/kdk135 once (pkgutil --expand-full) and KDKROOT points at its Payload; build.sh honours KDKROOT.
#  * build.sh wants jq gum xcodes cmake ninja bash: brew has the rest, `xcodes` is a no-op shim in ~/lavender-build/bin.
#  * Xcode: the workflow ends up on Xcode 16.0 (the oldest on the runner). 16.2 is what we try here; 16.0 is also installed
#    (/Applications/Xcode-16.0.app): set XCODE=/Applications/Xcode-16.0.app to build with the CI's exact toolchain.
# After the build, make the shim with the usual docker step (tools/build-shim.sh) on kernel.development.lavender.
set -euo pipefail
MAC=${MAC:-danielmiodowky@192.168.0.5}
XCODE=${XCODE:-/Applications/Xcode-16.2.app}
OUT=${1:-work/kernel-mac}
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549     # keep equal to .github/workflows/xnu.yml
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

git archive HEAD xnu/patches | tar -x -C "$TMP"
tar czf "$TMP/patches.tgz" -C "$TMP/xnu/patches" .
ssh -o BatchMode=yes "$MAC" 'mkdir -p ~/lavender-build/newp'
scp -q "$TMP/patches.tgz" "$MAC:lavender-build/patches.tgz"

ssh -o BatchMode=yes "$MAC" "XCODE='$XCODE' DXB_REPO='$DXB_REPO' DXB_SHA='$DXB_SHA' bash -s" <<'MAC_SCRIPT'
set -euo pipefail
export PATH=/opt/homebrew/bin:$PATH
cd ~/lavender-build
mkdir -p bin && printf '#!/bin/sh\nexit 0\n' > bin/xcodes && chmod +x bin/xcodes
for t in cmake ninja gum bash; do command -v $t >/dev/null || HOMEBREW_NO_AUTO_UPDATE=1 brew install $t; done
# the KDK is expanded once, without sudo
if [ ! -d ~/kdk135/KDK.pkg/Payload/System ]; then
  echo "KDK 13.5 not expanded in ~/kdk135: run build.sh once so it downloads /tmp/KDK.dmg, then:"
  echo "  hdiutil attach /tmp/KDK.dmg && pkgutil --expand-full '/Volumes/Kernel Debug Kit/KernelDebugKit.pkg' ~/kdk135"
  exit 1
fi
if [ ! -d dxb/.git ]; then
  rm -rf dxb && git init -q dxb && git -C dxb fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA" && git -C dxb checkout -q FETCH_HEAD
  sed -i '' 's/ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"/ARCHS="arm64" VALID_ARCHS="arm64" OTHER_CFLAGS="-mkernel -nostdinc -Wno-packed -mcpu=cortex-a53"/' dxb/build.sh
fi
rm -rf newp && mkdir newp && tar xzf patches.tgz -C newp
[ "$(grep -l $'\r' newp/*.patch | wc -l)" -eq 0 ] || { echo "CRLF in the patches"; exit 1; }
rm -f dxb/patches/[0-9]*.patch dxb/patches/[0-9]*.sh
cp newp/*.patch newp/*.sh dxb/patches/
cd dxb && rm -rf xnu build fakeroot && cd ..
export PATH=$HOME/lavender-build/bin:$PATH DEVELOPER_DIR=$XCODE/Contents/Developer KDKROOT=$HOME/kdk135/KDK.pkg/Payload
export KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG=LAVENDER MACOS_VERSION=13.5 MAKEFLAGS=-k
cd dxb && bash build.sh > ../build.log 2>&1
test -f fakeroot/System/Library/Kernels/kernel.development.lavender
MAC_SCRIPT

mkdir -p "$OUT"
scp -q "$MAC:lavender-build/dxb/fakeroot/System/Library/Kernels/kernel.development.lavender" "$OUT/"
ls -la "$OUT/kernel.development.lavender"
