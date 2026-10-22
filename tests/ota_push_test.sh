#!/bin/sh
# Push update path on the PC: real hassmic-host receiver, the PC's otatool (scripts/otatool.py) as ota-push.sh and
# adb-wifi.sh use it, and this script in the role of the root-side installer loop of main.sh (same commands, temp
# directories, the Echo's otatool: build/otatool-host).
cd "$(dirname "$0")/.."; T=$(mktemp -d); O="python3 scripts/otatool.py"; E=build/otatool-host; fail=0
ok() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fail=1; fi; }
$O keygen $T/k.sec $T/k.pub; $O keygen $T/evil.sec $T/evil.pub
mkdir $T/state
HASSMIC_STATE=$T/state HASSMIC_ADB_OPEN=$T/adb-open HASSMIC_UPDATE_PUB=$T/k.pub HASSMIC_SETTINGS=$T/settings build/hassmic-host -p 16971 -z 0 -o 16972 -L 2>$T/log &
PID=$!; for i in $(seq 1 200); do nc -z 127.0.0.1 16972 && break; sleep 0.1; done      # bound in ~0.3 s alone, later with every test at once
installer() {        # what ota_watch in main.sh does, once; $1: how long to wait for a request (1/4 s)
    for i in $(seq 1 ${1:-40}); do [ -f $T/state/ota/request ] && break; sleep 0.25; done
    [ -f $T/state/ota/request ] || return
    rm -f $T/state/ota/request
    if v=$($E install $T/k.pub $T/state/ota/bundle $T/state/ota/bundle.sig $T/installed 2>&1); then echo "OK $v" > $T/state/ota/result; else echo "FAILED $v" > $T/state/ota/result; fi
}
printf '#!/bin/sh\necho main\n' > $T/main.sh; cp build/hassmic-host $T/hassmic
$O pack $T/k.sec 9.9.9+test $T/good.bundle $T/main.sh $T/hassmic >/dev/null
installer & r=$($O push 127.0.0.1 16972 $T/good.bundle $T/good.bundle.sig); wait $!
[ "$r" = "OK 9.9.9+test" ] && [ -x $T/installed/hassmic ] && [ "$(cat $T/installed/VERSION)" = 9.9.9+test ]; ok $? "signed bundle is received, installed and reported: $r"
$O pack $T/evil.sec 6.6.6 $T/evil.bundle $T/main.sh >/dev/null
r=$($O push 127.0.0.1 16972 $T/evil.bundle $T/evil.bundle.sig); echo "$r" | grep -q "^FAILED signature" && [ ! -f $T/state/ota/request ]; ok $? "bundle from another key is refused before it reaches the installer: $r"
cp $T/good.bundle $T/bad.bundle; printf X | dd of=$T/bad.bundle bs=1 seek=30 conv=notrunc 2>/dev/null
r=$($O push 127.0.0.1 16972 $T/bad.bundle $T/good.bundle.sig); echo "$r" | grep -q "^FAILED signature"; ok $? "tampered bundle is refused: $r"
# One line only: the Echo reads a line, answers and closes, and a close with bytes left unread sends a reset, which
# throws away the answer if nc has not read it yet (under load it had not).  nc -N quits once the Echo closed.
# What fails on the port counts against the address it came from (three within a minute: turned away for a minute), so
# the probes below that fail on purpose come from addresses of their own on the loopback; 127.0.0.1 is the owner's PC.
# probe <source> <what to send, Python escapes> [secs] [stop at the first line]: the answer, as it came
probe() {
    python3 -c '
import socket, sys
s = socket.create_connection(("127.0.0.1", 16972), source_address=(sys.argv[1], 0)); s.settimeout(float(sys.argv[3]))
s.sendall(sys.argv[2].encode().decode("unicode_escape").encode("latin-1")); got = b""
try:
    while True:
        b = s.recv(200)
        if not b: break
        got += b
        if sys.argv[4] == "1" and b"\n" in got: break
except (socket.timeout, ConnectionResetError): pass
print(got.decode(errors="replace").strip())' "$1" "$2" "${3:-3}" "${4:-0}"
}
r=$(probe 127.0.0.2 'GET / HTTP/1.0\n'); echo "$r" | grep -q "^FAILED bad request"; ok $? "garbage on the port is refused: $r"

# adb over Wi-Fi with the update key.  This script is the firewall watcher: it takes the request and answers with adb-open.
watcher() {
    for i in $(seq 1 40); do [ -f $T/state/adb-request ] && break; sleep 0.25; done
    { read -r want; read -r src; } < $T/state/adb-request 2>/dev/null     # line 1: open (1); line 2: the one address that signed
    [ "$want" = 1 ] && [ "$src" = 127.0.0.1 ] && : > $T/adb-open; rm -f $T/state/adb-request
}
watcher & r=$($O adb 127.0.0.1 16972 $T/k.sec); wait $!
[ "$r" = "OK adb over Wi-Fi open for 30 min, for 127.0.0.1 only" ] && [ -f $T/adb-open ]; ok $? "update key opens adb over Wi-Fi, for the signer's address only: $r"
rm -f $T/adb-open
r=$($O adb 127.0.0.1 16972 $T/evil.sec); echo "$r" | grep -q "^FAILED signature" && [ ! -f $T/state/adb-request ]; ok $? "another key does not, and asks for nothing: $r"
# a recorded answer does not work again: the challenge differs each time
n1=$(probe 127.0.0.3 'HMOTA-ADB1\n' 2 1); n2=$(probe 127.0.0.4 'HMOTA-ADB1\n' 2 1)
echo "$n1" | grep -qE '^NONCE [0-9a-f]{64}$' && [ "$n1" != "$n2" ]; ok $? "a new challenge every time"
# Self test: a second of audio through the capture loop (the PC build reads a file), then the marker for root's
# installer loop, which makes the running update the factory copy (main.sh)
for i in $(seq 1 40); do [ -f $T/state/ota/healthy ] && break; sleep 0.25; done
[ -f $T/state/ota/healthy ] && grep -q "self test: passed" $T/log; ok $? "self test passed, installer told (state/ota/healthy)"
r=$(probe 127.0.0.5 'HMOTA-FACTORY1 9.9.9+test\n'); echo "$r" | grep -q "^FAILED bad request"; ok $? "no approval request any more: $r"
# One connection at a time: one trickling a byte every 3 s (each read well inside any per-read timeout) must still be
# cut off, and the push queued behind it go through
python3 -c '
import socket, select, time
s = socket.create_connection(("127.0.0.1", 16972), source_address=("127.0.0.6", 0)); t = time.time(); got = b""
while time.time() - t < 25 and not got:
    s.sendall(b"H"); got = s.recv(200) if select.select([s], [], [], 3)[0] else b""
print("%.0f %s" % (time.time() - t, got.decode().strip()))' > $T/trickle &
TR=$!; sleep 0.5
rm -rf $T/installed; installer 100 & r=$($O push 127.0.0.1 16972 $T/good.bundle $T/good.bundle.sig); wait $!; wait $TR
read secs why < $T/trickle
[ "$secs" -le 12 ] && [ "$why" = "FAILED bad request" ] && [ "$r" = "OK 9.9.9+test" ]; ok $? "a trickling connection is cut off after ${secs} s ($why), the push behind it: $r"
# A peer that keeps reconnecting: after three failures within a minute it is turned away at once, for a minute, and does not hold the port
# any more; the owner's PC is not affected
for i in 1 2 3; do probe 127.0.0.7 'nonsense\n' > /dev/null; done
t0=$(date +%s); r=$(probe 127.0.0.7 'HMOTA-ADB1\n' 5); t1=$(date +%s)
echo "$r" | grep -q "^FAILED too many failed connections from this address, try again in [0-9]* s" && [ $((t1 - t0)) -le 1 ] &&
    grep -q "update: 3 failed connections from 127.0.0.7 within 60 s: turned away for 60 s" $T/log
ok $? "three failures from 127.0.0.7: turned away at once ($((t1 - t0)) s): $r"
kill $PID; rm -rf $T
[ $fail = 0 ] && echo "all good" || { echo FAILED; exit 1; }
