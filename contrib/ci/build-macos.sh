#!/usr/bin/env bash
# Builds netcoind and the Qt wallet on macOS with Homebrew and packages them
# as dist/netcoin-<ver>-macos-<arch>.dmg (NetCoin.app with netcoind inside).
# Used by the GitHub workflows; see doc/build-macos.md.
set -euo pipefail

NCVER="${1:-dev}"
JOBS="$(sysctl -n hw.ncpu)"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARCH="$(uname -m)"
QT5="$(brew --prefix qt@5)"
LOG="$ROOT/build-macos.log"
: > "$LOG"
fail() {
    if [ -n "${GITHUB_ACTIONS:-}" ]; then
        local msg
        msg="$( (grep -E 'error:|Error [0-9]|Undefined symbols|ld: |No such file' "$LOG" | tail -15; echo '--- last lines ---'; tail -25 "$LOG") | sed 's/%/%25/g' | sed ':a;N;$!ba;s/\n/%0A/g')"
        echo "::error title=macOS build failed::${msg:0:60000}"
    fi
    exit 1
}
trap fail ERR
exec > >(tee -a "$LOG") 2>&1

cd "$ROOT/src"
make -f makefile.osx -j"$JOBS" netcoind test_netcoin
./test_netcoin

mkdir -p "$ROOT/build-qt"
cd "$ROOT/build-qt"
"$QT5/bin/qmake" ../netcoin-qt.pro USE_UPNP=1 USE_QRCODE=1 CONFIG+=release
make -j"$JOBS"

NAME="netcoin-$NCVER-macos-$ARCH"
DIST="$ROOT/dist/$NAME"
rm -rf "$DIST"
mkdir -p "$DIST"
cp -R netcoin.app "$DIST/NetCoin.app"
APP="$DIST/NetCoin.app"
cp "$ROOT/src/netcoind" "$APP/Contents/MacOS/"
# copy Qt and all Homebrew libraries into the bundle and point the binaries at them
"$QT5/bin/macdeployqt" "$APP" -executable="$APP/Contents/MacOS/netcoind" -always-overwrite
# nothing may still refer to Homebrew
if otool -L "$APP/Contents/MacOS/"* "$APP/Contents/Frameworks/"*.dylib | grep -E '/(opt|usr/local)/'; then
    echo "error: the bundle still links to Homebrew libraries"; false
fi
# macdeployqt invalidates the signatures; Apple Silicon needs at least an ad-hoc one
codesign --force --deep --sign - "$APP"
codesign --verify --deep --strict "$APP"
"$APP/Contents/MacOS/netcoind" -? | head -3

# smoke test of the bundled binaries (nobody tests the Mac build by hand):
# the node starts, answers RPC and stops cleanly; the wallet window starts
SMOKE="$(mktemp -d)"
printf 'rpcuser=u\nrpcpassword=p\nlisten=0\nlistenonion=0\nconnect=0\ndnsseed=0\n' > "$SMOKE/netcoin.conf"
"$APP/Contents/MacOS/netcoind" -testnet -datadir="$SMOKE" -daemon
for i in $(seq 1 30); do
    "$APP/Contents/MacOS/netcoind" -testnet -datadir="$SMOKE" getinfo >/dev/null 2>&1 && break
    sleep 2
done
"$APP/Contents/MacOS/netcoind" -testnet -datadir="$SMOKE" getinfo | grep -E '"version"|"blocks"'
"$APP/Contents/MacOS/netcoind" -testnet -datadir="$SMOKE" stop
sleep 5
mkdir -p "$SMOKE/gui"
QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORM_PLUGIN_PATH="$QT5/plugins/platforms" "$APP/Contents/MacOS/netcoin" -testnet -datadir="$SMOKE/gui" -splash=0 -connect=0 -dnsseed=0 &
GUIPID=$!
sleep 30
kill -0 "$GUIPID"          # still running after 30 s
grep -a "Done loading" "$SMOKE/gui/testnet/debug.log"
kill "$GUIPID"; wait "$GUIPID" || true
rm -rf "$SMOKE"

cp "$ROOT/COPYING" "$DIST/COPYING.txt"
cp "$ROOT/contrib/ci/README-macos.txt" "$DIST/README.txt"
ln -s /Applications "$DIST/Applications"
cd "$ROOT/dist"
hdiutil create -volname "NetCoin $NCVER" -srcfolder "$NAME" -ov -format UDZO "$NAME.dmg"
ls -la "$ROOT/dist"
trap - ERR
