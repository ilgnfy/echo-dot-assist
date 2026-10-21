# Sourced by the PC scripts after scripts/lib/device.sh (DEVICE, OUT): where the Echo's binaries in build/<codename>/
# come from.  Built here (make: the Android NDK and the unpacked firmware), or the release CI published of this very
# commit: hassmic-<codename>.bundle, checked against keys/release.pub and unpacked by scripts/otatool.py, so nothing
# has to be installed or compiled for it (the same files byte for byte).
#   PREBUILT=1    the release's; stop if this commit has none
#   PREBUILT=0    always build
#   (unset)       build where the NDK is there, else the release's
# $OUT/PREBUILT marks a release's files there: "<tag> <commit> <version>", then their names.  A build here removes them
# first, or make would take the downloaded binaries (newer than the sources) as up to date.

REPO=ilgnfy/echo-dot-assist             # where CI publishes this fork's releases

_head() { git rev-parse HEAD 2>/dev/null; }
# no changes to tracked files: only then is the release's build this tree's
_clean() { git diff --quiet HEAD -- 2>/dev/null; }

# prebuilt_tag: the release of HEAD.  CI tags v<commit time, UTC> (-beta from main) on the commit it built: the tag must
# point at HEAD itself.  Fails for a tree with changes, a commit not pushed, CI not done yet, or no git checkout.
prebuilt_tag() {
    _clean || return 1
    _h=$(_head) _v=$(TZ=UTC0 git log -1 --format=%cd --date=format-local:%Y.%m.%d.%H%M%S)
    for _t in v$_v-beta v$_v; do
        _s=$(curl -fsS --max-time 20 "https://api.github.com/repos/$REPO/git/ref/tags/$_t" 2>/dev/null |
             sed -n 's/.*"sha": *"\([0-9a-f]*\)".*/\1/p' | head -1)
        [ "$_s" = "$_h" ] && { echo "$_t"; return 0; }
    done
    return 1
}

# prebuilt_current: $OUT holds the release of HEAD already
prebuilt_current() {
    [ -f $OUT/PREBUILT ] && _clean && [ "$(sed -n '1s/^[^ ]* \([^ ]*\) .*/\1/p' $OUT/PREBUILT)" = "$(_head)" ]
}

# prebuilt_clear: a release's files out of $OUT (before a build here)
prebuilt_clear() {
    [ -f $OUT/PREBUILT ] || return 0
    for _f in $(sed 1d $OUT/PREBUILT); do rm -f "$OUT/$_f"; done
    rm -f $OUT/PREBUILT
}

# prebuilt_fetch TAG: download the release's bundle for this model, verify it, put its binaries into $OUT.  The scripts
# and device.conf in it are this checkout's own (same commit): those stay where they are.
prebuilt_fetch() {
    _d=$OUT/.prebuilt.$$ _b=hassmic-$DEVICE.bundle
    rm -rf $_d; mkdir -p $_d
    for _f in $_b $_b.sig; do
        curl -fsSL --retry 3 -o $_d/$_f "https://github.com/$REPO/releases/download/$1/$_f" ||
            { rm -rf $_d; echo "cannot download $_f of release $1" >&2; return 1; }
    done
    python3 scripts/otatool.py install keys/release.pub $_d/$_b $_d/$_b.sig $_d/files > /dev/null ||
        { rm -rf $_d; echo "release $1: $_b does not verify against keys/release.pub" >&2; return 1; }
    prebuilt_clear
    _names=
    for _f in $_d/files/*; do
        case ${_f##*/} in VERSION|device.conf|release.pub|hassmic.rc|*.sh) continue;; esac
        cp -p $_f $OUT/ && _names="$_names ${_f##*/}"
    done
    { echo "$1 $(_head) $(cat $_d/files/VERSION)"; printf '%s\n' $_names; } > $OUT/PREBUILT
    rm -rf $_d
    echo "release build $1 of this commit, verified:$_names"
}

# build_binaries: the device binaries into $OUT, from wherever PREBUILT says
build_binaries() {
    mkdir -p $OUT
    if [ "$PREBUILT" != 0 ]; then
        prebuilt_current && { echo "release build $(head -1 $OUT/PREBUILT | cut -d' ' -f1) of this commit"; return 0; }
        if [ "$PREBUILT" = 1 ] || [ ! -d toolchain/android-ndk-r21e ]; then
            if _t=$(prebuilt_tag); then prebuilt_fetch "$_t"; return; fi
            [ "$PREBUILT" = 1 ] && die "GitHub has no release build of this commit (changes here, not pushed, or CI not done yet); build it here: PREBUILT=0"
            echo "no release build of this commit on GitHub, and no Android NDK to build with: $DDIR/README.md" >&2
        fi
    fi
    prebuilt_clear
    make -s all DEVICE=$DEVICE
}

# build_version: what the hassmic in $OUT reports, and so what a bundle of it must be called
build_version() {
    if [ -f $OUT/PREBUILT ]; then head -1 $OUT/PREBUILT | cut -d' ' -f3; else make -s version; fi
}
