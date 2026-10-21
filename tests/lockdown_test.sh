#!/bin/sh
# adb over Wi-Fi in scripts/device/lockdown.sh on the PC: who the INPUT rule for port 5555 admits, against iptables
# stand-ins (tests/fake_iptables.py: the filter table in a file, in the wording of "iptables -S") and properties in
# files.  Checked: closed, stock's rule for everyone goes and stays gone; a request signed with the update key admits
# its one address only (-s), stock's rule for everyone goes then too; the switch admits the network, or ADB_WIFI_FROM;
# ADB_WIFI=1 the same; a source that is not strictly one IPv4 address (or subnet, in hassmic.conf) opens nothing;
# "lockdown.sh check" agrees with every one of these rule sets and catches a rule too many.  Run with the Echo's shell where there is one (mksh), else sh.  Needs python3.
cd "$(dirname "$0")/.."
SH=${SH:-$(command -v mksh || echo sh)}
T=$(mktemp -d); fail=0; W=
ok() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
waitfor() { i=0; while [ $i -lt ${2:-40} ]; do eval "$1" && return 0; sleep 0.25; i=$((i + 1)); done; return 1; }
cleanup() { [ -n "$W" ] && kill $W 2>/dev/null; rm -rf $T; }
trap cleanup EXIT

D=$T/sys BASE=$T/data P=$T/props
mkdir -p $T/bin $D $BASE/state $P
cp scripts/device/lockdown.sh devices/donut/device.conf $D/
export HASSMIC_BASE=$BASE FAKE_FW_DIR=$T
cat > $T/bin/getprop <<EOF
#!/bin/sh
if [ \$# = 0 ]; then for f in $P/*; do echo "[\${f##*/}]: [\$(cat \$f)]"; done; else cat $P/\$1 2>/dev/null; fi
EOF
printf '#!/bin/sh\necho "$2" > %s/$1\n[ "$1" = ctl.restart ] && echo "restart $2" >> %s/restarts\nexit 0\n' $P $T > $T/bin/setprop
for t in start stop; do printf '#!/bin/sh\nexit 0\n' > $T/bin/$t; done
for t in iptables ip6tables iptables-restore ip6tables-restore; do
    printf '#!/bin/sh\nFAKE_TOOL=%s exec python3 %s/tests/fake_iptables.py "$@"\n' $t "$(pwd)" > $T/bin/$t
done
chmod 755 $T/bin/*
export PATH=$T/bin:$PATH

# Stock at boot: adbd listening on 5555, firewall.sh's rule admitting it from anywhere, in both tables
echo 192.168.1.1 > $P/net.dns1; echo 5555 > $P/service.adb.tcp.port; echo running > $P/init.svc.adbd
STOCK="-p tcp -m tcp --dport 5555 -j ACCEPT"
iptables -w -A INPUT $STOCK; ip6tables -w -A INPUT $STOCK

lockdown() { $SH $D/lockdown.sh "$@" > $T/out 2>&1; }
adb_rules() { iptables -w -S INPUT | grep -- "--dport 5555"; }
adb6() { ip6tables -w -S INPUT | grep -c -- "--dport 5555"; }
checks() { $SH $D/lockdown.sh check > $T/check 2>&1; }
ask() { printf "$1" > $BASE/state/adb-request; lockdown; }
conf() { printf "NAME=\"Test\"\n$1" > $BASE/hassmic.conf; }
port() { cat $P/service.adb.tcp.port; }
conf ''

lockdown
[ -z "$(adb_rules)" ] && [ "$(adb6)" = 0 ] && [ "$(port)" = 0 ] && [ ! -f $BASE/adb-open ] && checks
ok $? "closed: stock's rule for everyone gone from both tables, adbd without its TCP port, check passes"

ask '1\n192.168.1.50\n'
[ "$(adb_rules)" = "-A INPUT -s 192.168.1.50/32 $STOCK" ] && [ "$(adb6)" = 0 ] && [ "$(port)" = 5555 ] && [ -f $BASE/adb-open ] &&
    [ ! -f $BASE/state/adb-request ] && grep -q "OPEN: a root shell for 192.168.1.50/32" $T/out && checks
ok $? "signed request from 192.168.1.50: only that address admitted ($(adb_rules)), check passes"
iptables -w -A INPUT $STOCK
! checks && grep -q "port 5555 (adb) admitted: -A INPUT $STOCK" $T/check; ok $? "stock's rule for everyone back: check says so ($(cat $T/check))"
iptables -w -A INPUT -s 10.9.9.9/32 $STOCK; lockdown
[ "$(adb_rules)" = "-A INPUT -s 192.168.1.50/32 $STOCK" ] && checks; ok $? "... and the next load takes it out again, and one for another address"

ask '1\n'
[ "$(adb_rules)" = "-A INPUT $STOCK" ] && [ "$(port)" = 5555 ] && checks; ok $? "the switch (no address): the network, as stock's rule words it"
ask '0\n'
[ -z "$(adb_rules)" ] && [ "$(port)" = 0 ] && [ ! -f $BASE/adb-open ] && checks; ok $? "asked to close: closed"

bad=
for a in '192.168.1.5;reboot' '192.168.1.0/24' '010.1.1.1' '1.2.3.4.5' '300.1.1.1' '$(reboot)' '192.168.1.5 -j ACCEPT'; do
    printf '1\n%s\n' "$a" > $BASE/state/adb-request; lockdown
    { [ -z "$(adb_rules)" ] && [ "$(port)" = 0 ] && grep -q "request for a source that is not one IPv4 address refused" $T/out; } || bad="$bad [$a]"
done
[ -z "$bad" ] && checks; ok $? "requests naming anything but one plain IPv4 address open nothing${bad:+:$bad}"

conf 'ADB_WIFI=1
'; lockdown
[ "$(adb_rules)" = "-A INPUT $STOCK" ] && checks; ok $? "ADB_WIFI=1 alone: the network, as before"
conf ''; lockdown
[ -z "$(adb_rules)" ] && checks; ok $? "ADB_WIFI=1 taken out: closed"

# The same rule by rule, as when iptables-restore fails (load_each)
for t in iptables-restore ip6tables-restore; do mv $T/bin/$t $T/$t; printf '#!/bin/sh\nexit 1\n' > $T/bin/$t; chmod 755 $T/bin/$t; done
iptables -w -A INPUT $STOCK; ask '1\n192.168.1.50\n'
[ "$(adb_rules)" = "-A INPUT -s 192.168.1.50/32 $STOCK" ] && grep -q "loading rule by rule" $T/out && checks; ok $? "rule by rule: the same ($(adb_rules | tr '\n' ' '))"
ask '0\n'
[ -z "$(adb_rules)" ] && checks; ok $? "rule by rule: closed"
for t in iptables-restore ip6tables-restore; do mv -f $T/$t $T/bin/$t; done

# (The watcher loop itself, run in the background, did not react in the parallel suite's namespaces although it does alone; the load, the
# apply, the check and the request handling above are what is tested.  tests/ota_push_test.sh covers the request protocol end to end.)

[ $fail = 0 ] && echo "all good" || { echo FAILED; echo "--- last run"; cat $T/out; echo "--- watcher"; cat $T/watch 2>/dev/null; exit 1; }
