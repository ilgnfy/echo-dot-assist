#!/usr/bin/env python3
"""The text edits scripts/debloat.sh makes to Android init files (tests/unit/debloat_edit_test.py).

    debloat_edit.py KIND SRC DST ARG...

    services  ARG = service names   delete each `service NAME ...` line and everything indented below it
    unstart   ARG = service names   delete the `start|stop|restart NAME` lines of those services
    unimport  ARG = file names      delete the `import NAME` lines
    unline    ARG = one regex       delete the lines it matches (not comments, not blank lines)

Init files are read with surrogateescape, so bytes that are not UTF-8 come back unchanged.  Every edit is idempotent."""
import re
import sys


def strip_services(text, names):
    names = set(names)
    out, skip = [], False
    for line in text.split("\n"):
        if line.startswith("service "):
            t = line.split()
            skip = len(t) > 1 and t[1] in names
        elif line and not line[0].isspace():
            skip = False                      # the next stanza (`on`, `import`, a comment at column 0, another service)
        if not skip:
            out.append(line)
    return "\n".join(out)


def strip_lines(text, kind, names):
    names = set(names)
    out = []
    for line in text.split("\n"):
        t = line.split("#")[0].split()
        if kind == "unstart" and len(t) == 2 and t[0] in ("start", "stop", "restart") and t[1] in names:
            continue
        if kind == "unimport" and len(t) == 2 and t[0] == "import" and t[1] in names:
            continue
        out.append(line)
    return "\n".join(out)


def strip_regex(text, pattern):
    rx = re.compile(pattern)
    return "\n".join(l for l in text.split("\n") if not (l.strip() and not l.strip().startswith("#") and rx.search(l)))


def main(argv):
    if len(argv) < 5 or argv[1] not in ("services", "unstart", "unimport", "unline"):
        sys.stderr.write(__doc__)
        return 2
    kind, src, dst, args = argv[1], argv[2], argv[3], argv[4:]
    text = open(src, errors="surrogateescape").read()
    if kind == "services":
        new = strip_services(text, args)
    elif kind == "unline":
        new = strip_regex(text, args[0])
    else:
        new = strip_lines(text, kind, args)
    open(dst, "w", errors="surrogateescape").write(new)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
