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
d = os.environ["HASSMIC_AVAHI_DIR"]
files = [os.path.join(d, "airplay-raop.service"), os.path.join(d, "airplay-ap2.service")]
for f in files: open(f, "w").write("<service-group/>\n")
def bye(*a):
    for f in files:
        try: os.unlink(f)
        except OSError: pass
    sys.exit(0)
signal.signal(signal.SIGTERM, bye)
fd = None; t = 0
while True:
    if os.path.exists(conf + ".play"):
        if fd is None:
            try: fd = os.open(pipe, os.O_WRONLY | os.O_NONBLOCK)
            except OSError: fd = None; time.sleep(0.1); continue
        n = 441                                         # 10 ms
        b = b"".join(struct.pack("<hh", v, v) for v in (int(8000 * math.sin(2 * math.pi * 440 * (t + i) / 44100)) for i in range(n)))
        t += n
        try: os.write(fd, b)
        except BlockingIOError: pass
        except OSError: os.close(fd); fd = None
        time.sleep(0.01)
    else:
        if fd is not None: os.close(fd); fd = None
        time.sleep(0.05)
