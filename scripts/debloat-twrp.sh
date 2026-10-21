#!/bin/bash
# Puts back what scripts/debloat.sh moved into /system/.debloat, from TWRP, for an Echo that no longer comes up usefully
# (no Wi-Fi, so no adb over Wi-Fi).  Needs adb over USB (the wires on the test pads, devices/donut/README.md "USB access") and
# the Echo in recovery (white ring: `adb reboot recovery`, or hold Volume Up while plugging in the power).
#
#   scripts/debloat-twrp.sh [-n] [--reboot]      -n: only list what would be put back
#
# It mounts the active slot's system partition, moves every file of /system/.debloat back to the path it came from (the folder
# mirrors the Echo's paths: .debloat/system/bin/x -> system/bin/x, .debloat/init.rc -> init.rc), never overwrites a file that is
# there, and unmounts.  It does NOT undo the edits of init files that debloat.sh keeps copies of (`block`, `unstart`, `unline`,
# `install`): those copies are in the same folder and are put back over the edited file only with --restore-edits.
# What was already purged is not in /system/.debloat: that comes back only from the backup (backup-*/system_a.img).
set -u
DRY=0; REBOOT=0; EDITS=0
for a in "$@"; do case $a in -n) DRY=1;; --reboot) REBOOT=1;; --restore-edits) EDITS=1;; *) echo "usage: $0 [-n] [--reboot] [--restore-edits]" >&2; exit 2;; esac; done
MNT=/mnt/hm_system
t() { adb shell "$@" | tr -d '\r'; }
[ "$(adb get-state 2>/dev/null)" = recovery ] || { echo "the Echo is not in recovery (adb get-state: $(adb get-state 2>&1)): hold Volume Up while plugging in the power, or adb reboot recovery" >&2; exit 1; }
SLOT=$(t 'bcbtool get_active' | tr -d '\n ')
case $SLOT in a|b) ;; *) echo "unexpected slot '$SLOT'" >&2; exit 1;; esac
t "mkdir -p $MNT; umount $MNT 2>/dev/null; mount -o rw \$(ls /dev/block/platform/*/by-name/system_$SLOT | head -1) $MNT"
t "[ -d $MNT/system/etc/init ]" || { echo "system_$SLOT does not look like the system partition" >&2; exit 1; }
echo "system_$SLOT mounted at $MNT"
Q=$MNT/system/.debloat
n=$(t "[ -d $Q ] && find $Q -type f | wc -l" | tr -d ' ')
[ "${n:-0}" -gt 0 ] || { echo "nothing in /system/.debloat on system_$SLOT (purged, or never applied)" >&2; t "umount $MNT"; exit 1; }
echo "$n files in the quarantine"
if [ $DRY = 1 ]; then t "cd $Q && find . -type f | head -40"; t "umount $MNT"; exit 0; fi
t "cd $Q && find . -type f | while read -r f; do
    r=\${f#./}; d=$MNT/\$r
    case \$r in system/*|*/*) ;; esac
    dest=$MNT/\$r
    mkdir -p \"\$(dirname \"\$dest\")\"
    if [ -e \"\$dest\" ]; then echo \"exists, left: \$r\"; else mv \"\$f\" \"\$dest\" && echo \"back: \$r\"; fi
done" | tail -n 12
left=$(t "find $Q -type f | wc -l" | tr -d ' ')
echo "$left files still in the quarantine (edited init files and replaced files; they are kept there on purpose)"
t "sync; umount $MNT" && echo "unmounted"
[ $REBOOT = 1 ] && { adb reboot; echo "rebooting"; } || echo "reboot with: adb reboot"
