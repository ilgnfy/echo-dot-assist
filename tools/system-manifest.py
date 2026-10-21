#!/usr/bin/env python3
"""Rebuilds devices/<codename>/system-manifest.tsv from what is on the Echo now.

    tools/system-manifest.py LIVE.txt OLD_MANIFEST.tsv OUT.tsv

LIVE.txt: one line per regular file or symbolic link of the running /system (the quarantine /system/.debloat left out), made on the Echo:
    find /system -path /system/.debloat -prune -o \\( -type f -o -type l \\) -print | while read -r f; do
      if [ -L "$f" ]; then echo "l|$f|$(readlink "$f")"; else echo "f|$f|$(stat -c %s "$f")"; fi; done
OLD_MANIFEST: the previous manifest (path, group, need, what, evidence per file).  Rows of files that are gone are dropped, rows that stay keep their
judgement.  Symbolic links get their own rows: a link is a use of its target (find -type f never shows one; pass 4 of 2026-10-21 moved the target of
twelve Wi-Fi HAL links and the Echo lost its Wi-Fi), so a link's target is never 'removable', and a link takes its target's need.

Columns of the result: path, kind (file|link), size or link target, group, need, what it is / why it is there, evidence.
need: required | by-name | platform | removable | unknown (device-logs/system-copy/manifest.py has the definitions; docs/cloud-free.md)."""
import collections, os, sys

live_path, old_path, out_path = sys.argv[1:4]
old = {}
for l in open(old_path):
    if l.startswith("#"): continue
    p = l.rstrip("\n").split("\t")
    if len(p) >= 7 and p[1] in ("file", "link"): old[p[0]] = p[3:7]     # this tool's own format (kind, size/target before the group)
    elif len(p) >= 5: old[p[0]] = p[1:5]

rows, links, files = [], {}, {}
for l in open(live_path):
    k, path, rest = l.rstrip("\n").split("|", 2)
    path = path[len("/system/"):]
    (links if k == "l" else files)[path] = rest

def rel(target, linkpath):
    t = target if target.startswith("/") else os.path.normpath(os.path.join("/system", os.path.dirname(linkpath), target))
    return t[len("/system/"):] if t.startswith("/system/") else t

targets = collections.defaultdict(list)
for p, t in links.items(): targets[rel(t, p)].append(p)

out = []
for p in sorted(files):
    g, need, what, ev = old.get(p, ["unlisted", "unknown", "not in the previous manifest", "name"])
    if p in targets and need == "removable":
        need, what, ev = "required", f"target of {len(targets[p])} symbolic links ({targets[p][0]} ...): a link is a use", "link"
    out.append((p, "file", files[p], g, need, what, ev))
for p in sorted(links):
    t = rel(links[p], p)
    fneed = {r[0]: (r[3], r[4]) for r in out if r[1] == "file"}
    if t in fneed: g, need = fneed[t]
    elif links[p] in ("toybox", "toolbox"): g, need = "platform tools", "platform"
    elif t.startswith("system/") or t.startswith("vendor/"): g, need = "links", "platform"
    else: g, need = "links", "platform"
    what = f"symbolic link to {links[p]}"
    out.append((p, "link", links[p], g, need, what, "link"))
out.sort()
with open(out_path, "w") as f:
    f.write("# Every file and symbolic link under /system of the cleaned Echo (see tools/system-manifest.py): path<TAB>kind<TAB>size or link target<TAB>group<TAB>need<TAB>what it is / why it is there<TAB>evidence.\n")
    f.write("# need: required | by-name | platform | removable | unknown (docs/cloud-free.md).  A symbolic link is a use of its target.\n")
    for r in out: f.write("\t".join(r) + "\n")
c = collections.Counter(r[4] for r in out)
print(len(out), "rows:", dict(c), file=sys.stderr)
for r in out:
    if r[4] in ("unknown", "removable"): print(r[0], r[4], r[5], file=sys.stderr)
