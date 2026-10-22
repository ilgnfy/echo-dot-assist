#!/usr/bin/env python3
"""Stand-in for shairport-sync (HASSMIC_SHAIRPORT): reads the -c configuration, copies it beside itself, writes the avahi
service files like tools/airplay/patches' backend, and while "a sender plays" (the file <conf>.play exists) writes a
440 Hz tone, S16 44.1 kHz stereo, at real time into the pipe; it ends on SIGTERM like the real one."""
import math, os, re, signal, struct, sys, time
conf = sys.argv[sys.argv.index("-c") + 1]
text = open(conf).read()
open(conf + ".seen", "w").write(text)
open(conf + ".pid", "w").write(str(os.getpid()))
pipe = re.search(r'pipe = \{\s*name = "([^"]+)"', text).group(1)
meta = re.search(r'metadata = \{[^}]*pipe_name = "([^"]+)"', text).group(1)       # the metadata pipe: title, artist, album as shairport-sync words them
d = os.environ["HASSMIC_AVAHI_DIR"]
files = [os.path.join(d, "airplay-raop.service"), os.path.join(d, "airplay-ap2.service")]
for f in files: open(f, "w").write("<service-group/>\n")
def bye(*a):
    for f in files:
        try: os.unlink(f)
        except OSError: pass
    sys.exit(0)
signal.signal(signal.SIGTERM, bye)
import base64
def item(typ, code, val=None):
    h = lambda s: s.encode().hex()
    if val is None: return f"<item><type>{h(typ)}</type><code>{h(code)}</code><length>0</length></item>\n".encode()
    b = val.encode()
    return f"<item><type>{h(typ)}</type><code>{h(code)}</code><length>{len(b)}</length>\n<data encoding=\"base64\">\n{base64.b64encode(b).decode()}</data></item>\n".encode()
mfd = None; told = False
fd = None; t = 0
while True:
    if os.path.exists(conf + ".play") and not told:     # a track begins: the bundle of its metadata (and a cover art item the Echo must throw away)
        if mfd is None:
            try: mfd = os.open(meta, os.O_WRONLY | os.O_NONBLOCK)
            except OSError: mfd = None
        if mfd is not None:
            said = open(conf + ".play").read().strip()                      # the test may put "title|artist|album" in the file
            t3 = (said.split("|") + ["", "", ""])[:3] if said else ["Black Hole Sun", "Soundgarden", "Superunknown"]
            try:
                os.write(mfd, item("ssnc", "mdst") + item("core", "minm", t3[0]) + item("core", "asar", t3[1]) + item("core", "asal", t3[2]) + item("ssnc", "mden"))
                told = True
            except OSError: pass
    if not os.path.exists(conf + ".play"):
        told = False
    if os.path.exists(conf + ".play"):
        if fd is None:
            try: fd = os.open(pipe, os.O_WRONLY | os.O_NONBLOCK)
            except OSError: fd = None; time.sleep(0.1); continue
        n = 441                                         # 10 ms
        b = b"".join(struct.pack("<hh", v, v) for v in (int(8000 * math.sin(2 * math.pi * 440 * (t + i) / 44100)) for i in range(n)))
        t += n
        try: os.write(fd, b[:3]); os.write(fd, b[3:])         # a read can end inside a frame
        except BlockingIOError: pass
        except OSError: os.close(fd); fd = None
        time.sleep(0.01)
    else:
        if fd is not None: os.close(fd); fd = None
        time.sleep(0.05)
