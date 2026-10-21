#!/bin/sh
# What the Echo's CPU and memory go to, live over adb (the same as Home Assistant's task manager entities, every 2 s
# instead of 10): CPU and memory of the whole Echo (/proc/stat over one second, /proc/meminfo), the busiest processes
# with their CPU and memory (/proc/<pid>/stat over the same second: the Dot 3's toolbox top has no batch mode and its ps
# knows neither -A nor -o, so neither gave CPU shares there), and hassmic's threads by name (each names itself as it
# starts; libpryon's workers are named for it, threadname.h) with their CPU over that second.  CPU shares are of all
# cores together, as in Home Assistant; memory is the resident set.  Needs only adb and awk, so it runs on Windows
# (Git Bash) too; Ctrl-C ends it.
#   scripts/top.sh [seconds]       refresh interval, 2 by default; ANDROID_SERIAL picks one of several Echos, as for adb
#   ONCE=1 scripts/top.sh          one look, no screen clearing (for a log)
#   TOPN=<n>                       how many processes to list, 15 by default
set -e
cd "$(dirname "$0")/.."
SECS=${1:-2}
case "$SECS" in ''|*[!0-9]*|0) echo "usage: scripts/top.sh [seconds]" >&2; exit 2;; esac
TOPN=${TOPN:-15}
case "$TOPN" in ''|*[!0-9]*) echo "TOPN must be a number" >&2; exit 2;; esac
command -v adb > /dev/null || { echo "adb not found" >&2; exit 1; }
[ "$(adb get-state 2>/dev/null)" = device ] || { echo "no Echo on adb (scripts/adb-wifi.sh opens it over Wi-Fi)" >&2; exit 1; }

# Runs on the Echo (mksh, toybox): two looks at the machine's jiffies, every process and hassmic's threads one second
# apart, then memory and load.  read instead of cat: no process per file (about 170 processes on a Dot 3).
# shellcheck disable=SC2016
DEV='P=$(pidof hassmic); P=${P%% *}
snap() { head -1 /proc/stat
    for t in /proc/[0-9]*; do l=; { read -r l < $t/stat; } 2>/dev/null; [ -n "$l" ] && echo "Q $l"; done
    [ -n "$P" ] || return 0
    for t in /proc/$P/task/[0-9]*; do l=; { read -r l < $t/stat; } 2>/dev/null; [ -n "$l" ] && echo "T $l"; done; }
echo "@1"; snap; sleep 1; echo "@2"; snap
echo "@M"; cat /proc/meminfo; echo "@L"; cat /proc/loadavg; echo "@P $P"'

render() {
    awk -v when="$(date +%H:%M:%S)" -v serial="${ANDROID_SERIAL:-adb}" -v topn="$TOPN" '
    function total(l,   f, n, i, s) { n = split(l, f, " "); s = 0; for (i = 2; i <= 9 && i <= n; i++) s += f[i]; return s }
    # one line of /proc/<pid>/stat ("Q ") or /proc/<pid>/task/<tid>/stat ("T "): the name in parentheses (may hold
    # spaces and parentheses), then the fields; utime+stime are the 14th and 15th, the RSS in 4 KB pages the 24th
    function stat(l, k, w,   name, rest, f, id) {
        name = substr(l, index(l, "(") + 1); rest = name
        while (match(rest, /\) /)) { rest = substr(rest, RSTART + 2); }
        name = substr(name, 1, length(name) - length(rest) - 2)
        split(rest, f, " "); id = substr(l, 3); sub(/ .*/, "", id)
        names[w, id] = name; ticks[w, k, id] = f[12] + f[13]; rss[w, id] = f[22]; seen[w, id] = 1
    }
    # the ids of kind W seen in both looks, busiest first, into id[] and c[] (CPU % of all cores); returns how many.
    # Equal CPU (most processes idle at 0): the larger memory first
    function busiest(w, d,   key, p, n, i, j, x) {
        n = 0; split("", id); split("", c)
        for (key in seen) {
            split(key, p, SUBSEP)
            if (p[1] != w || !((w, 1, p[2]) in ticks) || !((w, 2, p[2]) in ticks)) continue
            n++; id[n] = p[2]; c[n] = 100 * (ticks[w, 2, p[2]] - ticks[w, 1, p[2]]) / d
        }
        for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++)
            if (c[j] > c[i] || (c[j] == c[i] && rss[w, id[j]] > rss[w, id[i]])) { x = c[i]; c[i] = c[j]; c[j] = x; x = id[i]; id[i] = id[j]; id[j] = x }
        return n
    }
    /^@1$/ { sect = 1; next } /^@2$/ { sect = 2; next } /^@M$/ { sect = "m"; next } /^@L$/ { sect = "l"; next }
    /^@P/ { pid = $2; next }
    sect == 1 && /^cpu / { t1 = total($0); next } sect == 2 && /^cpu / { t2 = total($0); next }
    (sect == 1 || sect == 2) && /^Q [0-9]/ { stat($0, sect, "Q"); next }
    (sect == 1 || sect == 2) && /^T [0-9]/ { stat($0, sect, "T"); next }
    sect == "m" { mem[$1] = $2; next }
    sect == "l" { load = $1 " " $2 " " $3; next }
    END {
        d = t2 - t1; if (d <= 0) d = 1
        avail = ("MemAvailable:" in mem) ? mem["MemAvailable:"] : mem["MemFree:"] + mem["Buffers:"] + mem["Cached:"]
        printf "%s  %s   load %s\n", when, serial, load
        if (mem["MemTotal:"]) printf "memory %.1f %% used, %d MB available of %d MB\n", 100 * (mem["MemTotal:"] - avail) / mem["MemTotal:"], avail / 1024, mem["MemTotal:"] / 1024
        n = busiest("Q", d); busy = 0; for (i = 1; i <= n; i++) busy += c[i]
        printf "\n%d processes, %.1f %% CPU in all; busiest over 1 s (all cores = 100 %%):\n", n, busy
        printf "  %6s  %-16s %7s  %8s\n", "PID", "NAME", "CPU", "MEMORY"
        for (i = 1; i <= n && i <= topn; i++) printf "  %6s  %-16s %5.1f %%  %5.1f MB\n", id[i], names["Q", id[i]], c[i], rss["Q", id[i]] * 4 / 1024
        if (pid == "") { print "\nhassmic is not running"; exit }
        n = busiest("T", d)
        printf "\nhassmic (pid %s), %.1f MB, %d threads, CPU over 1 s (all cores = 100 %%):\n", pid, rss["Q", pid] * 4 / 1024, n
        for (i = 1; i <= n; i++) printf "  %6s  %-16s %5.1f %%\n", id[i], names["T", id[i]], c[i]
    }'
}

while :; do
    out=$(adb shell "$DEV" | tr -d '\r')
    if [ -n "$ONCE" ]; then echo "$out" | render; exit 0; fi
    screen=$(echo "$out" | render)
    printf '\033[H\033[2J%s\n' "$screen"
    [ "$SECS" -gt 1 ] && sleep $((SECS - 1))
done
