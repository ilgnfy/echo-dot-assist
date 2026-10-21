#!/bin/sh
# Packs and signs an update bundle from a build of one model: what scripts/ota-push.sh pushes and CI publishes.
#   [DEVICE=<codename>] scripts/bundle.sh KEY VERSION       -> build/<codename>/hassmic.bundle and hassmic.bundle.sig
# Needs that model's binaries in build/<codename>/ first: `make all`, or a release's (scripts/lib/build.sh).
# keys/release.pub rides along (the key CI signs its bundles with; the Echo itself trusts only the owner's update.pub).
set -e
cd "$(dirname "$0")/.."
. scripts/lib/device.sh; device_load
OUT=${BUNDLE_FROM:-$OUT}            # scripts/content-id.sh: packs from a build of its own
[ $# = 2 ] || die "usage: [DEVICE=<codename>] scripts/bundle.sh KEY VERSION"
# The Echo's shell keeps a CR as part of each word: one in the boot scripts and nothing starts after the next boot.
# .gitattributes keeps them out of a checkout; this catches a tree checked out before it, or an editor that put them back.
for f in scripts/system/*.sh scripts/device/*.sh $DDIR/device.conf $DDIR/hassmic.rc; do
    ! grep -q "$(printf '\r')" "$f" || die "$f has CR LF line endings: git add --renormalize . && git checkout -- ."
done
python3 scripts/otatool.py pack "$1" "$2" $OUT/hassmic.bundle \
    $OUT/hassmic $OUT/runas $OUT/otatool $(ls $OUT/latency $OUT/mixcap $OUT/mixplay $OUT/pryon_test 2>/dev/null) $DDIR/device.conf:644 \
    $(for k in $OUT/*.ko; do [ -f "$k" ] && echo "$k:644"; done) keys/release.pub:644 \
    scripts/system/main.sh scripts/system/boot.sh scripts/system/sysinstall.sh $DDIR/hassmic.rc:644 \
    scripts/device/lockdown.sh scripts/device/alexa-off.sh scripts/device/alexa-on.sh scripts/device/artifact-install.sh scripts/device/wifi.sh
