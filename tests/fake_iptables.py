#!/usr/bin/env python3
"""Stand-in for the Echo's iptables, ip6tables and their -restore tools, for tests/alexa_test.sh.

Keeps the filter table in a JSON file ($FAKE_FW_DIR/<tool>.json) and answers in the wording of "iptables -S", which is
what lockdown.sh loads from and compares with.  Covers what lockdown.sh calls: -S [chain], -N, -F, -A, -I [n], -D, -P,
and restore with --noflush (*filter, ":CHAIN POLICY", -A/-I/-D, COMMIT; a refused line commits nothing, rc 2).  Every
change is appended to $FAKE_FW_DIR/log as "fw <tool> <state of hassmic_out>", so the test can check the order of
firewall changes against service starts.  -w (wait for the lock) is the default here: a file lock serialises callers.
"""
import fcntl
import json
import os
import sys

BUILTIN = ("INPUT", "FORWARD", "OUTPUT")


def state_of(t):
    """What hassmic_out does: none, ota-only (only the updaters dropped), lock (egress lock), or other."""
    rules = t["chains"].get("hassmic_out")
    if rules is None or "-j hassmic_out" not in t["chains"]["OUTPUT"][:1]:
        return "none"
    if any("--uid-owner" in r for r in rules) and rules[-1] != "-j DROP":
        return "ota-only"
    if rules and rules[-1] == "-j DROP":
        return "lock"
    return "other"


def listing(t, chain=None):
    out = []
    chains = [chain] if chain else list(t["chains"])
    for c in chains:
        out.append(f"-P {c} {t['policy'][c]}" if c in BUILTIN else f"-N {c}")
    for c in chains:
        out += [f"-A {c} {r}" for r in t["chains"][c]]
    return "\n".join(out)


def apply(t, args):
    """One rule command; False if iptables would refuse it."""
    op, chain, rest = args[0], args[1] if len(args) > 1 else "", args[2:]
    ch = t["chains"]
    if op == "-N":
        if chain in ch:
            return False
        ch[chain] = []
        return True
    if chain not in ch:
        return False
    if op == "-F":
        ch[chain] = []
    elif op == "-P":
        t["policy"][chain] = rest[0]
    elif op == "-A":
        ch[chain].append(" ".join(rest))
    elif op == "-I":
        pos = 1
        if rest and rest[0].isdigit():
            pos, rest = int(rest[0]), rest[1:]
        ch[chain].insert(pos - 1, " ".join(rest))
    elif op == "-D":
        r = " ".join(rest)
        if r not in ch[chain]:
            return False
        ch[chain].remove(r)
    else:
        return False
    return True


def restore(t, text):
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or line == "*filter":
            continue
        if line == "COMMIT":
            return True
        if line.startswith(":"):
            name, policy = line[1:].split()[:2]
            if name in BUILTIN:
                if policy != "-":
                    t["policy"][name] = policy
            else:
                t["chains"][name] = []          # declared again = emptied, as with --noflush
            continue
        if not apply(t, line.split()):
            return False
    return False                                # no COMMIT: nothing


def main():
    tool = os.environ.get("FAKE_TOOL") or os.path.basename(sys.argv[0])
    table = tool.replace("-restore", "")
    d = os.environ["FAKE_FW_DIR"]
    path = os.path.join(d, table + ".json")
    args = [a for a in sys.argv[1:] if a not in ("-w", "--noflush")]
    with open(os.path.join(d, table + ".lock"), "w") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        try:
            with open(path) as f:
                t = json.load(f)
        except FileNotFoundError:
            t = {"policy": {c: "ACCEPT" for c in BUILTIN}, "chains": {c: [] for c in BUILTIN}}
        before = json.dumps(t, sort_keys=True)
        if tool.endswith("-restore"):
            if not restore(t, sys.stdin.read()):
                return 2
        elif args and args[0] == "-S":
            chain = args[1] if len(args) > 1 else None
            if chain and chain not in t["chains"]:
                return 1
            print(listing(t, chain))
            return 0
        elif not apply(t, args):
            return 1
        if json.dumps(t, sort_keys=True) != before:
            with open(path + ".tmp", "w") as f:
                json.dump(t, f)
            os.replace(path + ".tmp", path)
            with open(os.path.join(d, "log"), "a") as f:
                f.write(f"fw {table} {state_of(t)}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
