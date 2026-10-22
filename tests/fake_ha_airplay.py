#!/usr/bin/env python3
"""AirPlay 2 (airplay.c) against build/hassmic-host, as Home Assistant: off by default and nothing running; switched on
from Home Assistant, shairport-sync (tests/fake_shairport.py) runs with a configuration of ports in the admitted range
and the Echo's name, a sender's audio reaches the music stream, a gap ends it, a Sendspin-style newer source drops the
sender (the child restarts), switched off stops the child and takes its mDNS records out."""
import asyncio, os, re, sys, tempfile, time
from esphome_common import check, until, sync, home_assistant, run, ROOT
from aioesphomeapi import SwitchInfo, SwitchState

PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT = 17131, 17132, 17133, 17134, 17135
AVAHI = tempfile.mkdtemp()
MUSIC = tempfile.mktemp(suffix=".raw")


async def main(e):
    ha = await home_assistant(e)
    c, by, st = ha.cli, ha.by, ha.states
    conf = os.path.join(e.state, "airplay.conf")
    sw = by.get("airplay_2")
    check(isinstance(sw, SwitchInfo) and e.pv("airplay") == 0 and not os.path.exists(conf + ".pid"), "AirPlay off by default: the switch is there, no receiver runs")
    check(e.pv("airplay_delay") == 0, "the trim starts at 0 ms")
    e.page.set(airplay="on")                                   # a feature: Home Assistant's link is closed, it lists again
    await until(lambda: os.path.exists(conf + ".pid"), 8)
    check(os.path.exists(conf + ".pid") and e.conf().get("airplay") == "on", f"switched on from Home Assistant: the receiver runs, kept in the settings: {e.conf().get('airplay')}")
    seen = open(conf + ".seen").read()
    ports = [int(x) for x in re.findall(r"(?:port|udp_port_base) = (\d+)", seen)]
    check('name = "Echo Dot"' in seen and ports and all(16384 <= p <= 32767 for p in ports) and "udp_port_range = 40" in seen
          and "audio_backend_latency_offset_in_seconds = -0.191" in seen and 'output_rate = 44100' in seen,
          f"its configuration: the Echo's name, ports {ports} in the admitted range, the latency offset worked out (110 + 11 + 70 ms)")
    check(os.path.exists(os.path.join(AVAHI, "airplay-raop.service")) and os.path.exists(os.path.join(AVAHI, "airplay-ap2.service")), "its mDNS records are there")
    check(os.stat(os.path.join(e.state, "airplay.pipe")).st_mode & 0o777 == 0o600, "the FIFO is the daemon's alone (0600)")
    # a sender plays
    open(conf + ".play", "w").close()
    await until(lambda: os.path.exists(MUSIC) and os.path.getsize(MUSIC) > 44100 * 4 * 0.5, 10)
    sz = os.path.getsize(MUSIC) if os.path.exists(MUSIC) else 0
    check(sz > 44100 * 4 * 0.5, f"the sender's audio reaches the music stream ({sz} bytes)")
    import struct
    raw = open(MUSIC, "rb").read(); samples = struct.unpack(f"<{len(raw) // 2}h", raw[:len(raw) // 2 * 2])
    peak = max(abs(v) for v in samples)
    check(e.pv("airplay_gain") == 0 and 7500 < peak < 8500, f"AirPlay gain 0 by default: the stand-in's 8000 arrives as {peak}")
    check("ignore_volume_control = \"no\"" in open(conf).read(), "the sender's slider is AirPlay's volume (shairport-sync applies it)")
    # while it plays, the Echo's volume (40) does not scale it: the mixer is at the top, AirPlay's gain 1, the replies turned down from there
    rd = lambda n: int(open(os.path.join(MIXER, n)).read())
    await until(lambda: rd("MainVolume") == 100 and rd("TTSVolume") == 100, 5)
    check(rd("MainVolume") == 100 and rd("TTSVolume") == 100, f"playing: the mixer at the top ({rd('MainVolume')}, {rd('TTSVolume')}), not at the Echo's volume 40")
    # a gap ends the stream; played again it starts again
    os.unlink(conf + ".play")
    await asyncio.sleep(2.5)
    sz2 = os.path.getsize(MUSIC)
    await asyncio.sleep(1.0)
    check(os.path.getsize(MUSIC) == sz2, "a pause of the sender ends the stream: nothing more is written")
    await until(lambda: rd("MainVolume") == 40 and rd("TTSVolume") == 40, 5)
    check(rd("MainVolume") == 40 and rd("TTSVolume") == 40, f"AirPlay stopped: the mixer back at the Echo's volume ({rd('MainVolume')}, {rd('TTSVolume')})")
    # another source starts: the sender is dropped, the receiver restarts
    open(conf + ".play", "w").close()
    await until(lambda: os.path.getsize(MUSIC) > sz2 + 44100 * 4 * 0.5, 10)
    pid0 = open(conf + ".pid").read()
    e.proc.send_signal(__import__("signal").SIGHUP)             # the "stop" keyword (tests on the PC): music is paused, AirPlay's sender dropped
    await until(lambda: open(conf + ".pid").read() != pid0, 10)
    check(open(conf + ".pid").read() != pid0, "\"stop\" drops the sender: the receiver is started anew")
    os.unlink(conf + ".play")
    # off: the child ends, records gone
    e.page.set(airplay="off")
    await until(lambda: not os.path.exists(os.path.join(AVAHI, "airplay-raop.service")), 8)
    check(not os.path.exists(os.path.join(AVAHI, "airplay-raop.service")) and not os.path.exists(os.path.join(AVAHI, "airplay-ap2.service")) and e.conf().get("airplay") == "off",
          "switched off: the receiver stops and its mDNS records are gone")


MIXER = tempfile.mkdtemp()                       # the mixer's volumes (HASSMIC_FAKE_MIXER): the Echo's volume is 40
for _n in ("MainVolume", "TTSVolume"): open(os.path.join(MIXER, _n), "w").write("40\n")

run(main, (PORT, HTTP_PORT, WEB, HTTPS_PORT, SELF_PORT),
    {"HASSMIC_FAKE_MIXER": MIXER, "HASSMIC_SHAIRPORT": f"{ROOT}/tests/fake_shairport.py", "HASSMIC_AVAHI_DIR": AVAHI, "HASSMIC_MUSIC": MUSIC})
