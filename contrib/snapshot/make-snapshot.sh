#!/usr/bin/env bash
# Makes a block chain snapshot of a NetCoin node for "File > Import block
# chain snapshot" (or netcoind -loadsnapshot=<file>).
#
# The node is stopped while the files are packed (a few minutes) so that the
# database is consistent, then started again. Only the block files and the
# block index are packed: wallet.dat, the onion key, peers.dat and
# netcoin.conf are never included.
#
# usage: sudo contrib/snapshot/make-snapshot.sh [-d datadir] [-s service] [-o outdir]
#   -d  data directory (default: /var/lib/netcoin if it exists, else ~/.netcoin)
#   -s  systemd service that runs netcoind (default: found automatically;
#       without one, netcoind is stopped with "netcoind stop" and started
#       again with "netcoind -daemon")
#   -o  where to write the snapshot (default: current directory)
#   -b  netcoind binary (default: netcoind from PATH or /usr/local/bin)
#   -t  testnet (the block chain is in <datadir>/testnet)
set -euo pipefail

DATADIR=""
SERVICE=""
OUTDIR="$(pwd)"
NETCOIND="$(command -v netcoind || echo /usr/local/bin/netcoind)"
TESTNET=""
while getopts "d:s:o:b:th" opt; do
    case $opt in
        d) DATADIR="$OPTARG" ;;
        s) SERVICE="$OPTARG" ;;
        o) OUTDIR="$OPTARG" ;;
        b) NETCOIND="$OPTARG" ;;
        t) TESTNET="-testnet" ;;
        *) sed -n '2,20p' "$0"; exit 1 ;;
    esac
done
if [ -z "$DATADIR" ]; then
    if [ -d /var/lib/netcoin ]; then DATADIR=/var/lib/netcoin; else DATADIR="$HOME/.netcoin"; fi
fi
CHAINDIR="$DATADIR${TESTNET:+/testnet}"
[ -f "$CHAINDIR/blk0001.dat" ] && [ -d "$CHAINDIR/txleveldb" ] || { echo "no block chain in $CHAINDIR"; exit 1; }
command -v python3 >/dev/null || { echo "python3 is needed"; exit 1; }

# run netcoind commands as the owner of the data directory
OWNER="$(stat -c %U "$DATADIR")"
as_owner() {
    if [ "$(id -un)" = "$OWNER" ]; then "$@"; else sudo -u "$OWNER" "$@"; fi
}
cli() { as_owner "$NETCOIND" -datadir="$DATADIR" $TESTNET "$@"; }

if [ -z "$SERVICE" ] && [ -d /run/systemd/system ]; then
    SERVICE="$(timeout 20 systemctl list-units --type=service --all --no-legend 2>/dev/null | awk '{print $1}' | grep -i -m1 netcoin || true)"
fi

HEIGHT="$(cli getblockcount)"
HASH="$(cli getblockhash "$HEIGHT")"
NAME="netcoin-snapshot-$HEIGHT"
echo "best block: $HEIGHT $HASH"
echo "stopping the node${SERVICE:+ (service $SERVICE)}"
if [ -n "$SERVICE" ]; then
    systemctl stop "$SERVICE"
else
    cli stop
fi
# wait until netcoind has exited (it holds the database open until then)
for i in $(seq 1 120); do
    pgrep -u "$OWNER" -x netcoind >/dev/null || break
    sleep 1
done
restart() {
    echo "starting the node again"
    if [ -n "$SERVICE" ]; then systemctl start "$SERVICE"; else cli -daemon; fi
}
trap restart EXIT

# the block chain must not have moved since we read the height
echo "packing $CHAINDIR into $OUTDIR/$NAME.zip"
python3 - "$CHAINDIR" "$OUTDIR/$NAME.zip" "$HEIGHT" "$HASH" <<'EOF'
import os, sys, time, zipfile
datadir, out, height, blockhash = sys.argv[1:5]
tmp = out + ".part"
with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, allowZip64=True, compresslevel=6) as z:
    z.writestr("snapshot.txt", "netcoin-snapshot 1\nheight=%s\nhash=%s\ncreated=%s\n"
               % (height, blockhash, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())))
    for name in sorted(os.listdir(datadir)):
        if name.startswith("blk") and name.endswith(".dat") and len(name) == 11:
            print("  " + name, flush=True)
            z.write(os.path.join(datadir, name), name)
    z.writestr(zipfile.ZipInfo("txleveldb/"), "")
    for name in sorted(os.listdir(os.path.join(datadir, "txleveldb"))):
        if name in ("LOCK", "LOG", "LOG.old"):
            continue
        z.write(os.path.join(datadir, "txleveldb", name), "txleveldb/" + name)
    print("  txleveldb/", flush=True)
os.replace(tmp, out)
EOF
trap - EXIT
restart

cd "$OUTDIR"
# GitHub release files must be smaller than 2 GiB: split larger snapshots
# into parts (name.zip.001, .002, ...); the wallet reads them as one file
# when the first part is chosen
FILES="$NAME.zip"
if [ "$(stat -c %s "$NAME.zip")" -gt $((1900 * 1024 * 1024)) ]; then
    echo "splitting into parts of 1900 MB"
    rm -f "$NAME.zip".[0-9][0-9][0-9]
    split -b 1900M -d -a 3 --numeric-suffixes=1 "$NAME.zip" "$NAME.zip."
    rm -f "$NAME.zip"
    FILES="$(ls "$NAME.zip".[0-9][0-9][0-9] | tr '\n' ' ')"
fi
sha256sum $FILES | tee "$NAME.sha256"
ls -la $FILES
FIRST="${FILES%% *}"
cat <<EOF

Snapshot of block $HEIGHT ($HASH):
  $FILES
Publish these files and $NAME.sha256, e.g. as a GitHub release. To use it,
download all parts into one folder and choose $FIRST in
File > Import block chain snapshot, or: netcoind -loadsnapshot=$FIRST
EOF
