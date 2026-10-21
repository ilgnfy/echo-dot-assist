#!/bin/sh
# The root side of the boot logic on the PC: scripts/system/main.sh itself (not a copy of its commands, as
# ota_push_test.sh has) in temp directories, with the Echo's otatool (build/otatool-host) and stand-ins for Android's
# getprop/start/stop/pidof/chown.  Covers what decides whether an Echo keeps a working, locked-down boot: which bundles
# ota_watch installs, where root writes in the daemon's directory, when the start counter of an update is reset, and a
# config that does not load.  Run with the Echo's shell where there is one (mksh), else sh.
#   tests/boot_test.sh            needs build/otatool-host (make build/otatool-host) and python3
cd "$(dirname "$0")/.."
SH=${SH:-$(command -v mksh || echo sh)}
T=$(mktemp -d); O="python3 scripts/otatool.py"; fail=0; MAIN=
ok() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
cleanup() { [ -n "$MAIN" ] && kill $MAIN 2>/dev/null; kill $HM 2>/dev/null; rm -rf $T; }
trap cleanup EXIT
waitfor() { i=0; while [ $i -lt ${2:-40} ]; do eval "$1" && return 0; sleep 0.25; i=$((i + 1)); done; return 1; }
[ -x build/otatool-host ] || { echo "build/otatool-host missing: make build/otatool-host"; exit 1; }

# Android's tools: properties, init services, process lookup; chown to the daemon's user needs root
mkdir -p $T/bin
printf '#!/bin/sh\n[ "$1" = ro.product.device ] && echo testdev\n' > $T/bin/getprop
printf '#!/bin/sh\necho "start $*" >> %s/svc\n' $T > $T/bin/start
printf '#!/bin/sh\necho "stop $*" >> %s/svc\n' $T > $T/bin/stop
printf '#!/bin/sh\ncat %s/pidof 2>/dev/null\n' $T > $T/bin/pidof
printf '#!/bin/sh\necho "setprop $*" >> %s/svc\n' $T > $T/bin/setprop
printf '#!/bin/sh\nexit 0\n' > $T/bin/chown
chmod 755 $T/bin/*
export PATH=$T/bin:$PATH

# System partition (factory copy) and the copy main.sh runs from
SYS=$T/sys D=$T/run BASE=$T/data
mkdir -p $SYS $D $BASE/state/ota $BASE/ota
cp build/otatool-host $SYS/otatool
$O keygen $T/k.sec $SYS/update.pub; $O keygen $T/evil.sec $T/evil.pub
echo 1.0 > $SYS/VERSION
printf '#!/bin/sh\necho "boot $*" > %s/booted\n' $T > $SYS/boot.sh
printf 'PRODUCT=testdev\nDAEMON_USER=nobody\n' > $D/device.conf
printf '#!/bin/sh\necho "$*" > %s/lockdown\nexit 0\n' $T > $D/lockdown.sh
cp scripts/system/main.sh $D/main.sh
echo 'NAME="Test"' > $BASE/hassmic.conf
run() { HASSMIC_DIR=$D HASSMIC_SYS=$SYS HASSMIC_BASE=$BASE $SH $D/main.sh "$@" & MAIN=$!; }
stop_main() { kill $MAIN 2>/dev/null; wait $MAIN 2>/dev/null; MAIN=; }

# What a bundle carries: main.sh, device.conf, a hassmic that is a real program (its /proc/<pid>/exe is what root
# compares), runas that passes the self-check through, and sysinstall.sh standing in for writing the system partition.
B=$T/b; mkdir $B
printf '#!/system/bin/sh\necho main\n' > $B/main.sh
printf 'PRODUCT=testdev\nDAEMON_USER=nobody\n' > $B/device.conf
cp "$(command -v sleep)" $B/hassmic
printf '#!/bin/sh\nshift 2; [ "$2" = -T ] && exit 0; exec "$@"\n' > $B/runas
printf '#!/bin/sh\necho "factory $*" > %s/factory\n' $T > $B/sysinstall.sh
pack() { v=$1; shift; $O pack $T/k.sec $v $BASE/state/ota/bundle "$@" > /dev/null; }
request() { rm -f $BASE/state/ota/result; touch $BASE/state/ota/request; waitfor "[ -f $BASE/state/ota/result ]" 60; }

# 1. A good bundle, with the daemon's traps laid: result.tmp a link to a file root would create and hand over, and a
# test binary from deploy.sh that would go on running instead of the update.
pack 2.0 $B/main.sh $B/device.conf:644 $B/hassmic $B/runas $B/sysinstall.sh
ln -s $T/victim $BASE/state/ota/result.tmp
cp $B/hassmic $BASE/hassmic
run firewall; request
r=$(cat $BASE/state/ota/result 2>/dev/null)
[ "$r" = "OK 2.0" ]; ok $? "signed bundle installed: $r"
[ ! -e $T/victim ] && [ ! -L $BASE/state/ota/result ]; ok $? "root writes the result in its own directory, not through the daemon's link"
[ "$(cat $BASE/ota/current/VERSION)" = 2.0 ] && [ "$(cat $BASE/ota/tries)" = 0 ]; ok $? "made current, start counter 0"
[ ! -e $BASE/hassmic ]; ok $? "the test binary in /data no longer masks the update"
waitfor "[ -f $T/booted ]"; grep -q "setprop ctl.restart hassmic" $T/svc && grep -q "boot firewall" $T/booted; ok $? "satellite restarted, firewall script run anew"
wait $MAIN 2>/dev/null; MAIN=
cur=$(readlink $BASE/ota/current)

# 2. Bundles whose scripts would not run on the Echo are not installed
run firewall
printf '#!/system/bin/sh\r\nD=/system/hassmic\r\n' > $T/main.sh
pack 3.0 $T/main.sh $B/device.conf:644 $B/hassmic $B/runas; request
r=$(cat $BASE/state/ota/result); echo "$r" | grep -q "^FAILED version 3.0: main.sh has CR LF"; ok $? "CR LF main.sh refused: $r"
printf 'PRODUCT=testdev\r\n' > $T/device.conf
pack 3.1 $B/main.sh $T/device.conf:644 $B/hassmic $B/runas; request
r=$(cat $BASE/state/ota/result); echo "$r" | grep -q "^FAILED version 3.1: device.conf has CR LF"; ok $? "CR LF device.conf refused: $r"
printf 'if then fi (\n' > $T/main.sh
pack 3.2 $T/main.sh $B/device.conf:644 $B/hassmic $B/runas; request
r=$(cat $BASE/state/ota/result); echo "$r" | grep -q "^FAILED version 3.2: main.sh does not parse"; ok $? "main.sh that does not parse refused: $r"
$O pack $T/evil.sec 6.6.6 $BASE/state/ota/bundle $B/main.sh $B/device.conf:644 > /dev/null; request
r=$(cat $BASE/state/ota/result); echo "$r" | grep -q "^FAILED the signature"; ok $? "bundle from another key refused: $r"
[ "$(readlink $BASE/ota/current)" = "$cur" ]; ok $? "current still the good one"
# the daemon made state/ota/result a directory: rm -f leaves it, and mv would drop the result inside it
rm -f $BASE/state/ota/result; mkdir -p $BASE/state/ota/result/sub; request 2> /dev/null
[ -f $BASE/state/ota/result ] && ! [ -L $BASE/state/ota/result ] && grep -q "^FAILED the signature" $BASE/state/ota/result; ok $? "a directory in place of the result is replaced, not written into"

# 3. The start counter: reset by a passed self test of the update's own hassmic, not by the factory copy's
echo 2 > $BASE/ota/tries
cp "$(command -v sleep)" $T/factory-hassmic; $T/factory-hassmic 60 & HM=$!; echo $HM > $T/pidof
touch $BASE/state/ota/healthy; waitfor "[ ! -f $BASE/state/ota/healthy ]"; sleep 0.5
[ "$(cat $BASE/ota/tries)" = 2 ] && [ ! -f $T/factory ]; ok $? "self test of the factory copy: counter kept, nothing written"
kill $HM; wait $HM 2>/dev/null
$cur/hassmic 60 & HM=$!; echo $HM > $T/pidof
touch $BASE/state/ota/healthy; waitfor "[ -f $T/factory ]"
[ "$(cat $BASE/ota/tries)" = 0 ] && grep -q "factory $cur" $T/factory; ok $? "self test of the update: counter 0, it becomes the factory copy"
kill $HM; wait $HM 2>/dev/null; HM=
stop_main

# 4. The config: CR LF converted; one that does not load still gets the firewall up
printf 'NAME="Test"\r\n' > $BASE/hassmic.conf; rm -f $T/lockdown
run firewall; waitfor "[ -f $T/lockdown ]"
! grep -q "$(printf '\r')" $BASE/hassmic.conf && grep -q "CR LF line endings, converted" $BASE/boot.log; ok $? "CR LF config converted"
stop_main
printf 'NAME="Te\n' > $BASE/hassmic.conf; rm -f $T/lockdown
# the watcher of the version before, as a push update leaves it running (matched by command line, read with mksh's read -d)
sh -c 'sleep 30' lockdown.sh watch & W=$!
run firewall; waitfor "[ -f $T/lockdown ]"; sleep 0.3
[ "$(cat $T/lockdown)" = watch ] && grep -q "hassmic.conf does not load (shell syntax): no satellite" $BASE/boot.log; ok $? "config that does not load: firewall watcher runs regardless"
case "$SH" in *mksh) ! kill -0 $W 2>/dev/null; ok $? "... and only one: the previous watcher is stopped first";; esac
kill $W 2>/dev/null; wait $W 2>/dev/null
stop_main
echo 2 > $BASE/ota/tries
run satellite; waitfor "! kill -0 $MAIN 2>/dev/null"; kill $MAIN 2>/dev/null; wait $MAIN; rc=$?; MAIN=
[ $rc = 1 ] && [ "$(cat $BASE/ota/tries)" = 0 ]; ok $? "... and no satellite (rc $rc), not counted against the update"
printf 'NAME="Test"\nexit 0\n' > $BASE/hassmic.conf; rm -f $T/lockdown
run firewall; waitfor "[ -f $T/lockdown ]"; sleep 0.3
[ "$(cat $T/lockdown)" = watch ] && [ "$(grep -c "does not load" $BASE/boot.log)" = 3 ]; ok $? "an exit in the config does not end the firewall service"
stop_main

echo 'NAME="Test"' > $BASE/hassmic.conf
# 6. The satellite's mDNS service file: hassmic -S runs as the daemon's user (runas), and root writes the file in its own
# directory and renames it into the daemon's, so a link the daemon left there is replaced, not written through.
# The address wait wants an interface with a real MAC (/sys/class/net); without one it would take 120 s.
WLAN=; for i in /sys/class/net/*; do grep -q '[1-9a-f]' $i/address 2>/dev/null && { WLAN=${i##*/}; break; }; done
if [ -z "$WLAN" ] || ! command -v setsid > /dev/null; then echo "skip mDNS service file: no interface with a MAC, or no setsid"
else
    printf 'PRODUCT=testdev\nDAEMON_USER=nobody\nDAEMON_GROUPS=nogroup\nWLAN=%s\n' $WLAN > $D/device.conf
    printf '#!/bin/sh\necho "$*" >> %s/runas.log\n[ "$1" = -r ] && shift 2\nshift 2; exec "$@"\n' $T > $D/runas
    printf '#!/bin/sh\ncase "$*" in *-S*) echo "  <name>echo-test</name>"; exit 0;; *-G*|*-T*) exit 0;; esac\nexec sleep 30\n' > $D/hassmic
    printf '#!/bin/sh\nexit 0\n' > $D/alexa-off.sh
    for t in ifconfig pkill avahi-daemon; do printf '#!/bin/sh\nexit 0\n' > $T/bin/$t; done
    chmod 755 $D/runas $D/hassmic $T/bin/*
    export HASSMIC_AVAHI=$T/avahi
    mkdir -p $T/avahi/services; echo keep > $T/victim2; ln -s $T/victim2 $T/avahi/services/hassmic.service
    HASSMIC_DIR=$D HASSMIC_SYS=$SYS HASSMIC_BASE=$BASE setsid $SH $D/main.sh satellite & MAIN=$!
    waitfor "[ -f $T/avahi/services/hassmic.service ] && [ ! -L $T/avahi/services/hassmic.service ]" 60
    grep -q "^nobody nogroup $D/hassmic .*-S$" $T/runas.log; ok $? "hassmic -S runs as the daemon's user: $(grep -- '-S$' $T/runas.log)"
    [ "$(cat $T/victim2)" = keep ] && grep -q "<name>echo-test</name>" $T/avahi/services/hassmic.service; ok $? "the service file replaces the daemon's link, does not write through it"
    waitfor "grep -q host-name=echo-test $T/avahi/avahi-daemon.conf 2>/dev/null"; ok $? "avahi's host name from the file root wrote"
    kill -- -$MAIN 2>/dev/null; wait $MAIN 2>/dev/null; MAIN=
fi

# 7. The task manager's kill (kill_watch): a process asked for by pid and start time is ended; init, a wrong start time, and a plain-text request are refused
echo 'NAME="Test"' > $BASE/hassmic.conf
printf 'PRODUCT=testdev\nDAEMON_USER=nobody\n' > $D/device.conf
run firewall
sleep 100 & VP=$!
vstart=$(sed 's/.*) //' /proc/$VP/stat | cut -d' ' -f20)
printf '%s %s TERM\n' $VP $vstart > $BASE/state/kill-request
waitfor "[ -f $BASE/state/kill-result ]" 60; read -r kres < $BASE/state/kill-result 2>/dev/null
! kill -0 $VP 2>/dev/null && case "$kres" in OK*) true;; *) false;; esac; ok $? "kill request: the process ended ($kres)"
rm -f $BASE/state/kill-result; printf '1 1 KILL\n' > $BASE/state/kill-request
waitfor "[ -f $BASE/state/kill-result ]" 60; read -r kres < $BASE/state/kill-result 2>/dev/null
case "$kres" in "FAILED refused"*) true;; *) false;; esac; ok $? "init is refused ($kres)"
sleep 100 & VP=$!; rm -f $BASE/state/kill-result
printf '%s 1 TERM\n' $VP > $BASE/state/kill-request
waitfor "[ -f $BASE/state/kill-result ]" 60; read -r kres < $BASE/state/kill-result 2>/dev/null
kill -0 $VP 2>/dev/null && case "$kres" in "FAILED refused"*) true;; *) false;; esac; ok $? "a pid with another start time is not the process asked for ($kres)"
kill $VP 2>/dev/null; stop_main

[ $fail = 0 ] && echo "all good" || { echo FAILED; cat $BASE/boot.log; exit 1; }
