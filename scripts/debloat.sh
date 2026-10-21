#!/bin/bash
# Removes (apply) or restores (revert) the Amazon services listed in devices/<codename>/debloat.list on a connected Echo,
# over adb, as root.  Nothing is deleted: init files move to /system/.debloat/ and edited files are kept there as they were.
#
#   scripts/debloat.sh [-n] apply|revert|status|purge|verify [list]   -n: show what would happen, change nothing
#   verify: after the reboot that follows apply: the needed daemons run, no removed one does, the microphone delivers audio
#   purge: delete /system/.debloat for good (revert is impossible afterwards: only a backup or a reflash brings it back)
#
# Only the active slot is touched (/system is the running system partition); the other slot stays as it was.
# Needs `adb` (set ANDROID_SERIAL with more than one device) and python3 on the PC for the file edits.
set -u
cd "$(dirname "$0")/.." || exit 1

DRY=0; [ "${1:-}" = -n ] && { DRY=1; shift; }
CMD=${1:-status}
DEVICE=${DEVICE:-$(adb shell getprop ro.product.device 2>/dev/null | tr -d '\r' | sed 's/_puffin//')}
LIST=${2:-devices/${DEVICE:-donut}/debloat.list}
[ -f "$LIST" ] || { echo "no list: $LIST" >&2; exit 1; }
OTATOOL=/data/local/hassmic/otatool
KEEP=/system/.debloat
TMP=/data/local/tmp/debloat

sh_() { adb shell "$1" 2>&1 < /dev/null | tr -d '\r'; }
run() { if [ $DRY = 1 ]; then echo "  would: $1"; else sh_ "$1"; fi; }
exists() { [ "$(sh_ "[ -e '$1' ] && echo y")" = y ]; }

# The edits of init files are in scripts/lib/debloat_edit.py (tests/unit/debloat_edit_test.py):
#   strip_services SRC DST NAME...     a `service NAME` line and everything indented below it
#   strip_lines SRC DST unstart|unimport NAME...   `start|stop|restart NAME` lines, `import NAME` lines
#   strip_regex SRC DST REGEX          lines matching it (data folders and migration calls of removed services)
EDIT=$(pwd)/scripts/lib/debloat_edit.py
strip_services() { python3 -I "$EDIT" services "$1" "$2" "${@:3}"; }
strip_lines() { python3 -I "$EDIT" "$3" "$1" "$2" "${@:4}"; }
strip_regex() { python3 -I "$EDIT" unline "$1" "$2" "$3"; }

remount() { [ $DRY = 1 ] || sh_ "$OTATOOL remount $1 /system" > /dev/null; }
case $CMD in apply|revert|purge) [ $DRY = 1 ] || trap 'adb shell "'$OTATOOL' remount ro /system" >/dev/null 2>&1 < /dev/null' EXIT;; esac

case $CMD in
apply|revert|purge)
    [ "$(sh_ 'id -u')" = 0 ] || { echo "adb shell is not root" >&2; exit 1; }
    [ "$(sh_ 'getprop ro.product.device')" = "donut_puffin" ] || { echo "not a donut_puffin" >&2; exit 1; }
    remount rw
    [ $DRY = 1 ] || sh_ "mkdir -p $KEEP $TMP"
    ;;
esac

if [ "$CMD" = verify ]; then
    bad=0
    # what the satellite cannot do without (all Amazon's, all local): the audio engine, the light ring, the buttons, Wi-Fi, binder
    for p in hassmic mixer ledcontroller acebuttond wpa_supplicant servicemanager; do
        [ -n "$(sh_ "pidof $p")" ] && echo "up      $p" || { echo "MISSING $p"; bad=1; }
    done
    # what must not be there: every daemon whose init definition was taken away (a process of that name means a start path is left)
    running=$(sh_ "ps" | awk '{ print $NF }' | sed 's#.*/##')
    for p in PuffinApp puffinmrmd dacd SmartHomed commsd tokend fireos-dha perfmonitord perfhald perfrecoveryd otad ace_otad update_engine assetmgrd gadgetsd \
             ace_metric_daemon aceusagestatd minerva_service logmgr trackerd provisionerd neo-init neo-service-coordinator halo_regiond psbroker factory-reset; do
        echo "$running" | grep -qx "$p" && { echo "RUNNING $p (should be gone)"; bad=1; }
    done
    # the microphone: the mixer hangs 20 s per stream without perfmonitord unless the stub is in place; hassmic logs it as stalls
    log=$(sh_ "cat /data/local/hassmic/boot.log")
    stalls=$(echo "$log" | awk '/^== satellite start/ { n = 0; next } /micAsr delivers nothing/ { n++ } END { print n + 0 }')
    [ "$stalls" = 0 ] && echo "mic     delivers audio (no stalls since the satellite started)" || { echo "MIC     $stalls stalls since the satellite started: the mixer waits for a service that is gone"; bad=1; }
    [ "$(sh_ 'getprop sys.boot_completed')" = 1 ] && echo "boot    completed" || { echo "BOOT    not completed"; bad=1; }
    [ $bad = 0 ] && echo "verify: ok" || echo "verify: FAILED"
    exit $bad
fi

if [ "$CMD" = purge ]; then
    if [ $DRY = 1 ]; then echo "  would delete $KEEP ($(sh_ "du -sk $KEEP 2>/dev/null | cut -f1") kB)"; exit 0; fi
    before=$(sh_ "df /system | tail -n 1" | tr -s ' ' | cut -d' ' -f3)
    sh_ "rm -rf $KEEP $TMP"
    sync_=$(sh_ "sync; df /system | tail -n 1" | tr -s ' ' | cut -d' ' -f3)
    echo "purged $KEEP: /system used $before -> $sync_ kB"
    exit 0
fi

BATCH=$(mktemp); n=0
while read -r -u 3 act a rest; do
    case $act in ''|'#'*) continue;; esac
    case $CMD:$act in
    apply:install)
        # install <file on the PC> <path on the Echo>: our own file in place of a stock one (the stock one is kept once).
        # Written beside it and renamed over it: a library that a running process has mapped must not change under it.
        src=$a; dest=$rest
        [ -f "$src" ] || { echo "no $src (build it first)" >&2; continue; }
        if [ $DRY = 1 ]; then echo "install $src -> $dest"; continue; fi
        # already in place: replacing a library that a process has mapped leaves an unlinked inode behind, and the
        # partition then refuses to go read-only again ("Device or resource busy") until the next boot
        have=$(sh_ "sha1sum $dest 2>/dev/null" | cut -d' ' -f1); want=$(shasum -a 1 "$src" | cut -d' ' -f1)
        if [ "$have" = "$want" ]; then echo "same   $dest"; continue; fi
        adb push "$src" "$TMP/inst" > /dev/null 2>&1 < /dev/null
        sh_ "[ -e $KEEP$dest ] || { mkdir -p $KEEP\$(dirname $dest) && cp -p $dest $KEEP$dest; }; cat $TMP/inst > $dest.new && chmod 644 $dest.new && chown 0:0 $dest.new && mv $dest.new $dest"
        n=$((n + 1)) ;;
    revert:install)
        dest=$rest
        if exists "$KEEP$dest"; then
            echo "back   $dest"; run "cp -p $KEEP$dest $dest.new && mv $dest.new $dest && rm $KEEP$dest"; n=$((n + 1))
        fi ;;
    status:install)
        dest=$rest
        if exists "$KEEP$dest"; then echo "replaced $dest"; else echo "STOCK    $dest"; fi ;;
    apply:rc|apply:file)
        echo "mv $a" >> "$BATCH" ;;
    revert:rc|revert:file)
        echo "back $a" >> "$BATCH" ;;
    status:rc|status:file)
        if exists "$a"; then echo "PRESENT  $a"; elif exists "$KEEP$a"; then echo "removed  $a"; else echo "absent   $a"; fi ;;
    apply:block|apply:unstart|apply:unimport|apply:unline)
        f=$a; names=$rest
        [ -n "$names" ] || continue
        tmp=$(mktemp); out=$(mktemp)
        adb pull "$f" "$tmp" > /dev/null 2>&1 < /dev/null || { echo "cannot read $f" >&2; rm -f "$tmp" "$out"; continue; }
        case $act in block) strip_services "$tmp" "$out" $names;; unline) strip_regex "$tmp" "$out" "$names";; *) strip_lines "$tmp" "$out" $act $names;; esac
        if cmp -s "$tmp" "$out"; then echo "nodefs $f: $names already out"; rm -f "$tmp" "$out"; continue; fi
        echo "edit   $f: -$act $names"
        if [ $DRY = 1 ]; then diff "$tmp" "$out" | grep -c '^<' | sed 's/^/  would remove lines: /'; rm -f "$tmp" "$out"; continue; fi
        # the original is kept once, as it came from the firmware; later edits of the same file build on the current one
        exists "$KEEP$f" || { sh_ "mkdir -p $KEEP\$(dirname $f)"; sh_ "cp -p $f $KEEP$f"; }
        adb push "$out" "$TMP/edit" > /dev/null 2>&1 < /dev/null
        sh_ "cat $TMP/edit > $f"      # into the existing file: owner, mode and SELinux label stay
        rm -f "$tmp" "$out"; n=$((n + 1)) ;;
    revert:block|revert:unstart|revert:unimport|revert:unline)
        f=$a
        if exists "$KEEP$f"; then echo "back   $f"; run "cat $KEEP$f > $f && rm $KEEP$f"; n=$((n + 1)); fi ;;
    status:block|status:unstart|status:unimport|status:unline)
        [ $act = block ] || continue
        f=$a
        for s in $rest; do
            if [ "$(sh_ "grep -c '^service $s ' $f")" = 0 ]; then echo "removed  $f: $s"; else echo "PRESENT  $f: $s"; fi
        done ;;
    esac
done 3< <(cat "$LIST" "${LIST%.list}-files.list" 2>/dev/null)

# the moves, in one go on the Echo (a few hundred adb calls would take minutes)
if [ -s "$BATCH" ]; then
    cat > "$BATCH.sh" <<'EOS'
KEEP=/system/.debloat
# every path a symbolic link in /system points at (a link is a use; the Wi-Fi HAL names are links to libacehal_network_all.so)
LINKS=/data/local/tmp/debloat/linktargets
find /system -type l 2>/dev/null | while read -r l; do t=$(readlink "$l"); case $t in /*) echo "$t";; *) echo "$(dirname "$l")/$t";; esac; done | sort -u > $LINKS
while read -r op p; do
    case $op in
    mv)   if [ -e "$p" ]; then
              if grep -qx "$p" "$LINKS"; then echo "KEPT $p (a symbolic link points at it)"; continue; fi
              mkdir -p "$KEEP$(dirname $p)" && mv "$p" "$KEEP$p" && echo "moved $p" || echo "FAILED $p"
          fi ;;
    back) if [ -e "$KEEP$p" ]; then mv "$KEEP$p" "$p" && echo "back $p" || echo "FAILED $p"; fi ;;
    esac
done
EOS
    cnt=$(wc -l < "$BATCH" | tr -d ' ')
    if [ $DRY = 1 ]; then
        echo "  would move $cnt paths in one go"
    else
        adb push "$BATCH" "$TMP/batch.list" > /dev/null 2>&1 < /dev/null
        adb push "$BATCH.sh" "$TMP/batch.sh" > /dev/null 2>&1 < /dev/null
        res=$(adb shell "sh $TMP/batch.sh < $TMP/batch.list" 2>&1 < /dev/null | tr -d '\r')
        done_n=$(echo "$res" | grep -c -E '^(moved|back) ')
        echo "files done: $done_n of $cnt"; echo "$res" | grep -E "FAILED|KEPT"
        n=$((n + done_n))
    fi
fi
rm -f "$BATCH" "$BATCH.sh"

msg="$CMD: $n changes"; [ $DRY = 1 ] && msg="$msg (dry run)"; echo "$msg"
[ $DRY = 0 ] && [ "$CMD" = apply ] && sh_ "touch /system/.debloated" > /dev/null
[ $DRY = 0 ] && [ "$CMD" = revert ] && sh_ "rm -f /system/.debloated" > /dev/null
[ "$CMD" = apply ] && [ $DRY = 0 ] && [ $n -gt 0 ] && echo "reboot to take effect: adb reboot"
exit 0
