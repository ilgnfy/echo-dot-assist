# The original repo and this fork

Compared on 2026-10-22: `original/main` (github.com/0x0520/echo-dot-assist, last commit `4de5c35` of 2026-10-06, the build that ran on the
Echo until the evening of 2026-10-21) and this fork (`cloud-free`). Both grew from the same commit, `c7794a4` of 2026-10-05
("artifacts.sh: deregister only once the Echo is a satellite again"); since then the original has 50 commits of its own, the fork well over a hundred.

The Echo was installed from the original (Debian, 2026-10-17). On 2026-10-21 the fork's build was pushed to it by mistake of the line,
which is why entities were gone. This page says what each side has so nothing is lost by choosing one.

## In one paragraph

The fork is the original **without Amazon's cloud** (services removed, not stopped; no online updates, no Amazon downloads), **with** a settings page, a
second wake word engine, several more features, **and AirPlay 2 on the Echo itself**. Everything the original shows in Home Assistant is there again, except
what needed the cloud. Of the original's hardening nothing is missing but the module split (below).

## Taken from the original (ported, written again for the fork's code)

| what | where | when |
|---|---|---|
| Hardening of the root side (`main.sh`, `tests/boot_test.sh`), Sendspin, Bluetooth and network limits, adb for the signer's address only | `fix:` commits of 2026-10-05 | 2026-10-22 |
| Switches "Bluetooth phones: AAC" and "Bluetooth proxy: secure pairing only" (off by default) | `a2dp_codecs.c`, `ble.c` | 2026-10-22 |
| **Offline alarm clock**: three alarms set from Home Assistant, ring on the Echo without it, repeat, snooze, stop | `alarms.c`, `alarmtime.c`; entities "Alarm 1-3", "Alarm ringing", "Next alarm", "Snooze alarm", "Stop alarm" | 2026-10-22 |
| **Task manager**: CPU, memory, load, top processes, last kill, kill through root | `taskmgr.c`, `scripts/top.sh` | 2026-10-22 |
| **Client write queue**: a stalled Home Assistant link holds up no one | `outq.c`, from `6bd155d` | 2026-10-22 |
| Switch **"Wi-Fi setup over Bluetooth"** (on by default; off, the Echo never advertises) | `improv.c`; the fork's Improv itself is its own (automatic: offline 20 s, or the action button) | 2026-10-22 |
| **Network hardening**: arbitration candidates heard twice and bounded per address and per minute, update-port back-off per address, Wyoming allowlist (`-A`, `-W` is the settings page here), close-on-exec sockets, a Wyoming reply a client left open is ended | `b6e7727`, `0e808f6`, `9903b5e` | 2026-10-22 |
| **Core liveness (part of `335f41a`)**: replies as numbered streams (a media stop during a slow fetch no longer silences every later reply), TTS queue bounded at 1 MiB with back pressure, helper processes with a deadline and their process group killed, one process per 2 s instead of three for the volume poll, the earcon thread asleep instead of polling every 20 ms, the wake engine reset by the capture thread, no core lock for the pipeline timeout every block, core threads checked | `335f41a` | 2026-10-22 |
| **Improv hardening**: the action button authorizes only a connected phone and a disconnect takes it back; ATT PDUs longer than the MTU dropped | `f3cf607` | 2026-10-22 |
| Entities that were page-only in the fork: mic level, noise reduction, Bluetooth announcements and language, debug access, join arbitration, Music Assistant without pairing, sound detection, Wi-Fi motion, SoC temperature, CPU usage | `proto_esphome.c` | 2026-10-22 |

## Only in the original (what the fork still lacks)

| feature | where | state in the fork |
|---|---|---|
| The rest of **core locking and liveness** (`335f41a`): microphone audio through a queue and a sender thread of its own, so noise reduction and gain run outside `core_lock` | `main.c` | **ported** (2026-10-23, below) |
| Sendspin and Bluetooth hardening, Wyoming allowlist, push-port back-off, arbitration limits | `fix:` commits of 2026-10-05 | **ported** (above) |
| Alexa on without a reboot | `scripts/alexa-on.sh` | not wanted any more (cloud-free) |
| Online updates from GitHub, "Download from Amazon", whisper detection | `update.c` and others | removed on purpose (cloud-free) |
| Downgrade protection for release-signed bundles | `ota.c`, `main.sh` | not needed: the fork trusts only the owner's key |
| `main.c` split into modules (`core_int.h`, `mic.c`, `playback.c`, `wakedet.c`, `wakewords.c`, `spawn.c`), `a2dp.c` split into link/SDP/sink/AVRCP/source | refactor | a different structure; not ported (would conflict with every fork change) |
| `make lint`, pinned CI actions, dev container, Windows install docs (WSL2) | CI, docs | the fork has its own CI; Windows Git Bash setup |

## What was left out, and why

**Microphone audio through a queue and a sender thread (part of `335f41a`), ported 2026-10-23.** In the original the capture thread, which also feeds the wake word,
used to do noise reduction, gain and the write to the client under `core_lock`; a stalled client made the wake word deaf. The fork had the write queue per connection
(`outq.c`) but still ran RNNoise and the gain, a few milliseconds per 30 ms block, under the lock. Now the capture thread only takes our own sounds out (wake sound
removal, which the original does not have) and queues 64 ms blocks (2 s, oldest dropped, logged as "mic: N ms dropped"); the thread `mic_sender` in `main.c` runs
noise reduction and gain on its own copy of the settings and takes the lock only to send. The second of room before a command still primes RNNoise (as `ahead` blocks),
a block carries the "hold the gain" flag of the sound it was cut in, and a pipeline that ended drops what is still queued (`mic_seq`). The PC suites pass unchanged
(the noise reduction, gain, replay-ahead and arbitration tests all go through it).

**The module split** (`core_int.h`, `mic.c`, `playback.c`, `wakedet.c`, `wakewords.c`, `spawn.c`, `a2dp.c` into five) is a different structure of the same code. It would
conflict with every change the fork has made since, and gains nothing for a user.

**Downgrade protection for release-signed bundles, Alexa-on without a reboot, online updates, the Amazon download:** not wanted or not possible in a fork that trusts only
its owner's key and talks to no cloud.

## Only in the fork (what the original lacks)

- **AirPlay 2** on the Echo itself (iPhone, iPad, Mac): shairport-sync and the PTP clock nqptp run on the Echo, one more music source, switch "AirPlay 2" (off by
  default), automatic lip sync. `docs/airplay.md`.
- **Cloud-free** (`docs/cloud-free.md`): Amazon's services removed instead of stopped; the Amazon download, whisper detection and online updates removed.
- **Settings page** on port 28931: login by the action button, settings by name, export/import and presets, copying settings and models between Echos,
  the network-wide login, a log viewer, rename, Identify, Wi-Fi switch; light ring designs and theme packs.
- **Wake word**: microWakeWord and Home Assistant as engines, a second wake word at once, "Wake word" event entity, no freeze after 18.6 h, "<wake word>, stop" pauses music.
- **Arbitration** with a learned window, Home Assistant tags for the key handoff, Kiosk Satellite mode, join without "perform actions".
- **Drop In** (calls between Echos, with a voice blueprint) and its tests against a real Home Assistant.
- **Timers**: ringing state, "Stop ringing", "any request stops a ring", their own volume; **separate volumes** for music/replies/sounds/timers/calls.
- **Light ring** as a Home Assistant light, the listening ring turned towards the talker, the Button event entity, speaker gain, wake sound removal.
- **Media over HTTPS**, Sendspin re-announce, state files that survive a hard reset, time zone from Home Assistant, a time on every log line, new-IP handling.
- **Setup**: guided setup for the Dot 3, Windows (Git Bash) support.
- Tests: `tests/run.py` runs everything in network namespaces, in CI, a real-Home-Assistant job.

## Entities in Home Assistant

| entity | original `4de5c35` | fork now |
|---|---|---|
| Media Player, Mute, Do not disturb, Wake sound, LED brightness / auto, Equalizer x3, Bluetooth pairing, Sendspin token | yes | yes |
| Mic level, Noise reduction, Bluetooth announcements and language, Debug access, Join arbitration, Arbitration peers, Music Assistant without pairing | yes | yes |
| Sound detection switch, Sound (event); Wi-Fi motion switch, sensor, sensitivity | yes | yes (the rest of a feature while it is on) |
| SoC temperature, CPU usage | yes | yes (diagnostic, disabled by default) |
| Alarm 1-3, Alarm ringing, Next alarm, Snooze alarm, Stop alarm | yes | yes |
| Memory used / available, Load average, Top processes, Last kill | yes | yes |
| Bluetooth AAC, Bluetooth proxy: secure pairing only, Wi-Fi setup over Bluetooth | yes | yes |
| Illuminance | yes | yes (feature "Light sensor", on by default as in the original) |
| Bluetooth speaker group (search, play on, status, delay) | yes | when the feature is on |
| Firmware (update, with an install button) | yes | a text sensor with the version (online updates removed) |
| Online updates (select) | yes | **removed** (GitHub) |
| Last request whispered | yes | **removed** (needed Amazon's download) |
| Timer ringing, Ringing timers, Stop ringing | no | yes |
| Drop In, Drop In with, End Drop In, Drop In result, action `drop_in` | no | yes, kept for now |
| Light ring, Button event, Wake word event, Identify, Restart, Factory reset, Web UI address, Assistant/Sounds/Timer/Call volume | no | yes |
| **AirPlay 2** (switch) | no | yes |
