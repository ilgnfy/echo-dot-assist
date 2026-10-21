#!/bin/bash
# Raw backup of a connected, rooted Echo over adb: every partition but userdata (a mounted ext4 cannot be copied in one
# piece; /data goes into a tar instead), each checked against the sha1sum the Echo itself computes.  The first thing to do
# before scripts/debloat.sh: persist/tee*/kb/dkb hold the device's keys and cannot be made again.
#
#   scripts/backup.sh [-n] [-o DIR] [-p name,name,...]
#     -n   list what would be copied and stop
#     -o   target folder (default device-logs/backup-<codename>-<date>/, git-ignored)
#     -p   only these partitions (by their by-name names), no /data tar: for trying it out
#
# Copies in 32 MiB pieces, so it can be stopped and run again: it carries on where the file ended.  adb over Wi-Fi closes
# itself after 30 min (lockdown.sh) and drops a running copy: when a piece brings nothing, the window is opened again with
# scripts/adb-wifi.sh (needs secrets/update.key) and the copy goes on.  About 2.3 GB for a Dot 3: 30-60 min over Wi-Fi.
set -u
cd "$(dirname "$0")/.." || exit 1

DRY=0; ONLY=; OUTDIR=
while [ $# -gt 0 ]; do
    case $1 in -n) DRY=1;; -o) OUTDIR=$2; shift;; -p) ONLY=$2; shift;; *) echo "usage: $0 [-n] [-o DIR] [-p names]" >&2; exit 2;; esac
    shift
done

sha1() { if command -v sha1sum > /dev/null; then sha1sum "$1" | cut -d' ' -f1; else shasum -a 1 "$1" | cut -d' ' -f1; fi; }
sha256_all() { if command -v sha256sum > /dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
fsize() { wc -c < "$1" | tr -d ' '; }
sh_() { adb shell "$1" 2>&1 < /dev/null | tr -d '\r'; }
reopen() {
    [ -f secrets/update.key ] && [ -n "${ECHO_IP:-}" ] && scripts/adb-wifi.sh "$ECHO_IP" > /dev/null 2>&1
    case ${ANDROID_SERIAL:-} in *:*) adb disconnect "$ANDROID_SERIAL" > /dev/null 2>&1; sleep 2; adb connect "$ANDROID_SERIAL" > /dev/null 2>&1;; esac
}

[ "$(sh_ 'id -u')" = 0 ] || { echo "adb shell is not root: connect to the Echo first" >&2; exit 1; }
DEVICE=$(sh_ 'getprop ro.product.device' | sed 's/_puffin$//')
case ${ANDROID_SERIAL:-} in *:*) ECHO_IP=${ANDROID_SERIAL%:*};; *) ECHO_IP=;; esac
[ -n "$OUTDIR" ] || OUTDIR=device-logs/backup-$DEVICE-$(date +%Y%m%d-%H%M)

# name -> block device and size, from the Echo's own tables
byname=$(sh_ 'ls -l /dev/block/platform/*/by-name 2>/dev/null' | sed -n 's/.* \([A-Za-z0-9_]*\) -> \(\/dev\/block\/[A-Za-z0-9]*\)$/\1 \2/p')
[ -n "$byname" ] || { echo "no partition table found under /dev/block/platform/*/by-name" >&2; exit 1; }
parts=$(sh_ 'cat /proc/partitions' | awk '{ print $4, $3 }' 2>/dev/null || true)
[ -n "$parts" ] || parts=$(sh_ 'cat /proc/partitions' | while read -r maj min blocks name; do echo "$name $blocks"; done)

list=()
while read -r name dev; do
    [ -n "$name" ] || continue
    case $name in userdata) continue;; esac
    [ -z "$ONLY" ] || case ",$ONLY," in *",$name,"*) ;; *) continue;; esac
    blocks=$(echo "$parts" | while read -r n b; do [ "$n" = "${dev#/dev/block/}" ] && echo "$b"; done | head -1)
    [ -n "$blocks" ] || continue
    list+=("$name $dev $((blocks * 1024))")
done <<< "$byname"
# the boot partitions of the eMMC itself (preloader): not in by-name for every model
if [ -z "$ONLY" ]; then
    for b in mmcblk0boot0 mmcblk0boot1; do
        blocks=$(echo "$parts" | while read -r n k; do [ "$n" = "$b" ] && echo "$k"; done | head -1)
        [ -n "$blocks" ] && ! echo "$byname" | grep -q " /dev/block/$b$" && list+=("$b /dev/block/$b $((blocks * 1024))")
    done
fi

total=0; for e in "${list[@]}"; do set -- $e; total=$((total + $3)); done
echo "backing up $DEVICE: ${#list[@]} partitions, $((total / 1048576)) MiB (+ /data as a tar) into $OUTDIR"
for e in "${list[@]}"; do set -- $e; printf '  %-14s %-26s %8d KiB\n' "$1" "$2" $(($3 / 1024)); done
[ $DRY = 1 ] && exit 0
mkdir -p "$OUTDIR" || exit 1
sh_ 'getprop; echo; cat /proc/mounts; echo; cat /proc/cmdline; echo; ls -l /dev/block/platform/*/by-name' > "$OUTDIR/device-info.txt"

CHUNK=33554432
fail=0
for e in "${list[@]}"; do
    set -- $e; name=$1; dev=$2; size=$3
    out=$OUTDIR/$name.img; part=$out.part
    if [ -f "$out" ] && [ "$(fsize "$out")" = "$size" ]; then echo "have   $name"; continue; fi
    : >> "$part"
    stalls=0
    while [ "$(fsize "$part")" -lt "$size" ]; do
        have=$(fsize "$part")
        if [ $((have % 1048576)) -ne 0 ]; then               # a piece that was cut: drop the partial megabyte
            dd if="$part" of="$part.t" bs=1048576 count=$((have / 1048576)) 2> /dev/null && mv "$part.t" "$part"; have=$(fsize "$part")
        fi
        adb exec-out "dd if=$dev bs=1048576 skip=$((have / 1048576)) count=$((CHUNK / 1048576)) 2>/dev/null" >> "$part" < /dev/null
        if [ "$(fsize "$part")" = "$have" ]; then
            stalls=$((stalls + 1)); echo "stall  $name at $((have / 1048576)) MiB, reopening adb ($stalls)"
            [ $stalls -ge 8 ] && { echo "giving up on $name" >&2; fail=1; break; }
            reopen; sleep 3
        else stalls=0; fi
    done
    [ $fail = 0 ] || break
    if [ "$(fsize "$part")" -gt "$size" ]; then dd if="$part" of="$out" bs=1048576 count=$((size / 1048576)) 2> /dev/null && rm -f "$part"; else mv "$part" "$out"; fi
    printf 'done   %s\n' "$name"
done
[ $fail = 0 ] || exit 1

if [ -z "$ONLY" ]; then
    echo "tar    /data"
    # toybox tar writes "removing leading '/' from member names" into the stream: the first line goes (measured, 39 bytes)
    adb exec-out "cd / && tar -cf - data 2>/dev/null" < /dev/null > "$OUTDIR/data.tar"
    if [ "$(head -c 8 "$OUTDIR/data.tar" | tr -d '\0' | head -c 8)" = "removing" ]; then tail -c +40 "$OUTDIR/data.tar" > "$OUTDIR/data.fixed" && mv "$OUTDIR/data.fixed" "$OUTDIR/data.tar"; fi
    tar -tf "$OUTDIR/data.tar" > /dev/null 2>&1 || { echo "data.tar is not a valid tar" >&2; fail=1; }
fi

# every partition against what the Echo itself computes (sha1sum is all toybox has)
bad=0
for e in "${list[@]}"; do
    set -- $e; name=$1; dev=$2
    want=$(sh_ "sha1sum $dev" | cut -d' ' -f1); have=$(sha1 "$OUTDIR/$name.img")
    if [ "$want" = "$have" ]; then echo "match  $name"; else echo "DIFFER $name (Echo $want, copy $have)" >&2; bad=1; fi
done
( cd "$OUTDIR" && sha256_all *.img $( [ -f data.tar ] && echo data.tar ) > SHA256SUMS )
cat > "$OUTDIR/RESTORE.md" <<EOF
# Backup of $DEVICE, $(date +%Y-%m-%d)

Raw dumps of every eMMC partition but userdata, made over adb from the running Echo (slot $(sh_ 'getprop ro.boot.slot_suffix')), each
verified against sha1sum run on the Echo. \`SHA256SUMS\`: \`shasum -a 256 -c SHA256SUMS\`. \`data.tar\` is /data. \`device-info.txt\`
has the partition table (by-name -> mmcblk0pN) and the properties.

persist, tee1/tee2, kb, dkb hold device keys and cannot be regenerated. Keep this folder; it is git-ignored on purpose.

Restore (TWRP, adb over USB; hold Volume Up while plugging in the power; the USB wires are on test pads, see devices/$DEVICE/README.md):

    adb push system_a.img /data/local/tmp/ && adb shell dd if=/data/local/tmp/system_a.img of=/dev/block/mmcblk0p13 bs=1M

with the node of that partition from device-info.txt. Do not write preloader, tee*, lk_* or persist unless they are damaged: a wrong
write there needs the bootrom exploit (kamakiri, a Linux PC). Data: \`adb shell tar -xf /data/local/tmp/data.tar -C /\` as root, hassmic stopped.
EOF
[ $bad = 0 ] && [ $fail = 0 ] && { echo "backup complete and verified: $OUTDIR"; exit 0; }
echo "backup NOT verified: see the lines above" >&2; exit 1
