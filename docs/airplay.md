# AirPlay 2 on the Echo Dot 3

The Echo shows up as an AirPlay 2 speaker (setting "AirPlay 2", off by default). It is one more music source beside Sendspin
and Bluetooth: the newest wins, `core_music(MUSIC_AIRPLAY)`; a source that starts drops the sender of the other
(`airplay_yield`: shairport-sync is restarted, the phone shows "disconnected"), and "<wake word>, stop" does the same.
Music Assistant can also bridge plain AirPlay (version 1) to an Echo; this is the native receiver, which keeps in step with other
AirPlay 2 speakers through the clock nqptp shares.

## Pieces

| piece | where | what |
|---|---|---|
| `airplay.c` | hassmic | writes `state/airplay.conf`, runs shairport-sync as its child (restart 2 s, then 30 s after five quick exits), reads its audio from the FIFO `state/airplay.pipe` (mode 0600) into the music stream (S16 44.1 kHz stereo), 1.5 s without audio ends it |
| shairport-sync `b991d91` | `devices/donut/prebuilt/airplay/` | AirPlay 2 receiver, FFmpeg decoders, output to the pipe; one patch (`tools/airplay/patches`): the "external-avahi" mDNS backend writes the stock avahi-daemon's service files (`airplay-raop.service`, `airplay-ap2.service` beside hassmic's own) with the text records AirPlay 2 needs |
| nqptp 1.2.9 | same | the PTP clock (UDP 319, 320; shared memory in `/dev/shm`); root's `main.sh` (`airplay_clock`, netwatch's 10 s loop) runs it only while `state/config` says `airplay=on` |
| `lockdown.sh` | root | admits UDP 319 and 320 from private and link-local ranges, only while `airplay=on` (`airplay_state`, like Kiosk Satellite's port); `check` catches them left open |

Ports: RTSP 28933, UDP 28940-28979 (shairport-sync's "any port" sockets), all in the range the firewall admits already.
Name: the Echo's name ("Echo Dot 3 5695c4"). The sender's volume is applied by shairport-sync in software; hassmic's music
volume (and the separate volumes) apply on top, as for every music source.

## Why glibc, static

The firmware's C library (bionic) has no `pthread_cancel`, which shairport-sync uses throughout, and no `shm_open`. A statically linked
glibc binary needs no loader and runs on the Echo's kernel as it is (`tools/airplay/build.sh`; Debian's `gcc-arm-linux-gnueabihf`,
hard-float ARMv7 with VFPv3-D16, which the MT8167 has). `getaddrinfo` and `getpwnam` in a static glibc want the shared libraries at run time;
neither is used for anything that matters here (peers are addresses, no user lookups). `tools/airplay/cross-libs.sh` (`GNU=1`) builds the libraries
(OpenSSL 3.0, libsodium, libgpg-error + libgcrypt, libplist, libconfig, popt, libuuid and a minimal FFmpeg 6.1: AAC and ALAC decoders).
Two build snags are in the scripts' comments: libgpg-error has no lock description for the hard-float triplet (the generated one
has a 4-byte lock and the program stops at start with "gpgrt fatal: sizeof lock obj"), and libgcrypt and OpenSSL both define `gf_mul`.

## Delay

The audio reaches the mixer through a queue of 120 ms (as Sendspin's) and then the speaker. shairport-sync is told to hand audio out
early by the setting "AirPlay delay" (`audio_backend_latency_offset_in_seconds`), 250 ms to start with: to be found by ear against a
second AirPlay speaker playing the same song (not measured yet, PLAN.md).

## Security

Anyone on the network can play to an AirPlay speaker, as with every one; there is no pairing here (shairport-sync's AirPlay 2 uses
transient pairing). The receiver runs as the daemon's user, the clock as root: nqptp only reads UDP and writes shared memory.
Neither reaches the internet (firewall: the daemon's real group may reach local addresses and what hassmic fetches; root's nqptp
answers peers only). Off: no process, no ports, no mDNS records.

## Tests

`tests/fake_ha_airplay.py` (`tests/fake_shairport.py` plays the receiver's part: reads the configuration, writes the records, a tone into the pipe):
off by default, on from Home Assistant's link, configuration and ports, records, audio into the music stream, a gap, "stop", off.
`tests/lockdown_test.sh`: the two ports. A real sender is needed for the rest (pyatv cannot: it times with NTP, AirPlay 2 receivers want PTP).
