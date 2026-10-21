#!/usr/bin/env python3
"""Which files can go once the services in devices/<codename>/debloat.list have lost their init definitions.

    tools/debloat-analyze.py SYSTEM_DIR [--rc DIR]... [--hassmic FILE_OR_DIR]... [--list LIST] [--out FILE] [--keep PATH]...

SYSTEM_DIR   the unpacked firmware's /system (firmware/<codename>/rootfs/system), or a copy pulled from the Echo.
--rc DIR     where the init files are (searched for *.rc, recursively); default: SYSTEM_DIR/.. and SYSTEM_DIR/etc/init, SYSTEM_DIR/vendor/etc/init.
             Pass the files of the *unmodified* firmware: the services to remove are looked up in them.
--hassmic    our own executables and scripts (build/<codename>/*, scripts/system, devices/<codename>): library names inside them are roots,
             since hassmic dlopen()s some of the stock libraries by name (libcurl, the A2DP codecs, libacehal_dha, ...).
--list       the removal list (default devices/donut/debloat.list): `rc FILE` and `block FILE NAME...` lines name the services.
--out        write the result as a `file <path>` list (devices/<codename>/debloat-files.list format), else print a summary.
--keep       never delete this path (relative to SYSTEM_DIR, e.g. bin/firewall.sh); more than once.

How it decides (docs/cloud-free.md has the reasoning):
 1. executables = what the removed services' command lines run, the scripts they start, and Amazon's own test tools;
 2. libraries = every lib/*.so that no remaining file can reach: a file reaches a library when the library's name stands in it
    (DT_NEEDED and dlopen() strings alike), starting from every executable that stays, vendor/, etc/ and hassmic;
    names built at run time (lib%s_%s.so) keep everything their pattern matches.
It does not look inside a library for code, and it does not know what a script does with an executable it calls by a name it builds:
the removal is always tested on the Echo (scripts/debloat.sh verify, a cold boot, the wake word).
"""
import argparse, collections, fnmatch, glob, json, os, re, sys

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument("system")
ap.add_argument("--rc", action="append", default=[])
ap.add_argument("--hassmic", action="append", default=[])
ap.add_argument("--list", default="devices/donut/debloat.list")
ap.add_argument("--out")
ap.add_argument("--keep", action="append", default=[])
args = ap.parse_args()
SYS = os.path.abspath(args.system)

# ---- 1. the services to remove, from the list, and their command lines from the init files
removed_files, removed_names = set(), set()
for line in open(args.list):
    p = line.split()
    if not p or p[0].startswith("#"): continue
    if p[0] == "rc": removed_files.add(p[1])
    elif p[0] == "block": removed_names.update(p[2:])

rc_dirs = args.rc or [os.path.dirname(SYS), SYS + "/etc/init", SYS + "/vendor/etc/init"]
rc_files = []
for d in rc_dirs:
    for dp, _, fs in os.walk(d):
        if dp.count("/") - d.count("/") > 3: continue
        rc_files += [os.path.join(dp, f) for f in fs if f.endswith(".rc")]

services = []                    # (file as the Echo names it, service name, command line)
for f in sorted(set(rc_files)):
    base = os.path.basename(f)
    e_path = None
    for cand in ("/" + base, "/system/etc/init/" + base, "/vendor/etc/init/" + base):
        if cand in removed_files: e_path = cand
    for line in open(f, errors="replace").read().split("\n"):
        if line.startswith("service "):
            t = line.split()
            if len(t) >= 3: services.append((e_path, t[1], " ".join(t[2:])))
for e_path, name, cmd in services:
    if e_path: removed_names.add(name)

bins = set()
for e_path, name, cmd in services:
    if name not in removed_names: continue
    for tok in cmd.replace('"', " ").split():
        m = re.match(r"^/(system|vendor)/bin/([\w.+-]+)$", tok)
        if not m: continue
        rel = ("bin/" if m.group(1) == "system" else "vendor/bin/") + m.group(2)
        if os.path.exists(f"{SYS}/{rel}"): bins.add(rel)

# Amazon's test tools (they pull in most of the libraries nothing else uses)
test_pat = re.compile(r"(_test$|_tests$|test_app$|test_client$|test_server$|_test_|^ace_.*tests?$)")
if os.path.isdir(SYS + "/bin"):
    for f in os.listdir(SYS + "/bin"):
        if test_pat.search(f) and os.path.isfile(f"{SYS}/bin/{f}"): bins.add("bin/" + f)
KEEP = set(args.keep) | {"bin/firewall.sh", "bin/pre_factory_reset.sh", "bin/dhcpcd", "bin/wpa_supplicant", "bin/mixer", "bin/netmgrd", "bin/wifisvc"}
bins -= KEEP

# ---- 2. libraries nothing that stays can reach
def names_in(path):
    try: b = open(path, "rb").read()
    except OSError: return set(), set()
    names = {m.group(0).decode() for m in re.finditer(rb"[A-Za-z0-9_.+-]*lib[A-Za-z0-9_.+-]*\.so(?:\.[0-9]+)*", b)}
    pats = {m.group(0).decode() for m in re.finditer(rb"lib[A-Za-z0-9_.+-]*%s[A-Za-z0-9_.+%-]*\.so", b)}
    return names, pats

files = []
for dp, _, fs in os.walk(SYS):
    for f in fs:
        p = os.path.join(dp, f)
        if os.path.isfile(p) and not os.path.islink(p): files.append(os.path.relpath(p, SYS))
libs = {f[4:]: f for f in files if f.startswith("lib/") and "/" not in f[4:] and f.endswith(".so")}

roots = [os.path.join(SYS, f) for f in files if f not in bins and f.split("/")[0] in ("bin", "vendor", "xbin", "etc")]
for h in args.hassmic:
    if os.path.isdir(h): roots += [os.path.join(dp, f) for dp, _, fs in os.walk(h) for f in fs]
    elif os.path.exists(h): roots.append(h)

cache, needed, queue, patterns = {}, set(), [], collections.defaultdict(set)
def scan(path):
    if path not in cache: cache[path] = names_in(path)
    names, pats = cache[path]
    for n in names:
        if n in libs and libs[n] not in needed: needed.add(libs[n]); queue.append(libs[n])
    for q in pats: patterns[q].add(path)
for r in roots: scan(r)
while queue: scan(os.path.join(SYS, queue.pop()))
for q in list(patterns):
    g = re.sub(r"%[sd]", "*", q)
    for n, f in libs.items():
        if fnmatch.fnmatch(n, g) and f not in needed: needed.add(f); scan(os.path.join(SYS, f))
# A symbolic link is a use of its target: libacehal_wifi*.so, libacehal_network.so ... are links to libacehal_network_all.so, and `find -type f`
# (or os.walk with isfile) never shows a link. Taking the target away broke the Echo's Wi-Fi manager (2026-10-21).
linked = set()
for dp, dn, fs in os.walk(SYS):
    for f in fs + dn:
        lp = os.path.join(dp, f)
        if os.path.islink(lp):
            t = os.readlink(lp)
            linked.add(os.path.relpath(os.path.normpath(t if os.path.isabs(t) else os.path.join(dp, t)), SYS))
bins -= linked
dead_libs = sorted(f for f in libs.values() if f not in needed and f not in linked)

size = lambda f: os.path.getsize(f"{SYS}/{f}")
print(f"services to remove: {len(removed_names)}; executables: {len(bins)} ({sum(map(size, bins)) // 1024} kB); "
      f"libraries nothing reaches: {len(dead_libs)} ({sum(map(size, dead_libs)) // 1024} kB)", file=sys.stderr)
if args.out:
    out = ["# Generated by tools/debloat-analyze.py from the firmware's /system: executables the removed services run, Amazon's",
           "# test tools, and the libraries no remaining file can reach.  See the tool's docstring and docs/cloud-free.md."]
    out += ["", "# executables and scripts"] + ["file /system/" + f for f in sorted(bins)]
    out += ["", "# libraries"] + ["file /system/" + f for f in dead_libs]
    open(args.out, "w").write("\n".join(out) + "\n")
    print("wrote", args.out, file=sys.stderr)
else:
    for f in sorted(bins) + dead_libs: print(f)
