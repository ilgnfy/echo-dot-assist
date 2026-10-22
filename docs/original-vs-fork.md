# The original repo and this fork

Compared on 2026-10-22: `original/main` (github.com/0x0520/echo-dot-assist, last commit `4de5c35` of 2026-10-06, the build that ran on the
Echo until the evening of 2026-10-21) and this fork (`cloud-free`). Both grew from the same commit, `c7794a4` of 2026-10-05
("artifacts.sh: deregister only once the Echo is a satellite again"); since then the original has 50 commits of its own, the fork well over a hundred.

The Echo was installed from the original (Debian, 2026-10-17). On 2026-10-21 the fork's build was pushed to it by mistake of the line,
which is why entities were gone. This page says what each side has so nothing is lost by choosing one.

## In one paragraph

The fork is the original **without Amazon's cloud** (services removed, not stopped; no online updates, no Amazon downloads), **with** a settings page, a
second wake word engine, several more features, **and AirPlay 2 on the Echo itself**. Everything the original shows in Home Assistant is there again, except
what needed the cloud. What is still missing is the original's hardening of the satellite core (below).

## Taken from the original (ported, written again for the fork's code)

| what | where | when |
|---|---|---|
| Hardening of the root side (`main.sh`, `tests/boot_test.sh`), Sendspin, Bluetooth and network limits, adb for the signer's address only | `fix:` commits of 2026-10-05 | 2026-10-22 |
| Switches "Bluetooth phones: AAC" and "Bluetooth proxy: secure pairing only" (off by default) | `a2dp_codecs.c`, `ble.c` | 2026-10-22 |
| **Offline alarm clock**: three alarms set from Home Assistant, ring on the Echo without it, repeat, snooze, stop | `alarms.c`, `alarmtime.c`; entities "Alarm 1-3", "Alarm ringing", "Next alarm", "Snooze alarm", "Stop alarm" | 2026-10-22 |
| **Task manager**: CPU, memory, load, top processes, last kill, kill through root | `taskmgr.c`, `scripts/top.sh` | 2026-10-22 |
| **Client write queue**: a stalled Home Assistant link holds up no one | `outq.c`, from `6bd155d` | 2026-10-22 |
| Switch **"Wi-Fi setup over Bluetooth"** (on by default; off, the Echo never advertises) | `improv.c`; the fork's Improv itself is its own (automatic: offline 20 s, or the action button) | 2026-10-22 |
| Entities that were page-only in the fork: mic level, noise reduction, Bluetooth announcements and language, debug access, join arbitration, Music Assistant without pairing, sound detection, Wi-Fi motion, SoC temperature, CPU usage | `proto_esphome.c` | 2026-10-22 |

## Only in the original (what the fork still lacks)

| feature | where | state in the fork |
|---|---|---|
| **Core locking and liveness fixes** | `335f41a`, about 500 lines of `main.c` | **not ported**: see "What the open items are" below |
| Arbitration candidate limits, bounded network peers, Wyoming allowlist, push-port back-off, Sendspin and Bluetooth hardening against hostile peers | `fix:` commits of 2026-10-05 | partly taken (above); **to review** one by one |
| Improv hardening | `improv.c` | the fork has its own `improv.c` |
| Alexa on without a reboot | `scripts/alexa-on.sh` | not wanted any more (cloud-free) |
| Online updates from GitHub, "Download from Amazon", whisper detection | `update.c` and others | removed on purpose (cloud-free) |
| Downgrade protection for release-signed bundles | `ota.c`, `main.sh` | not needed: the fork trusts only the owner's key |
| `main.c` split into modules (`core_int.h`, `mic.c`, `playback.c`, `wakedet.c`, `wakewords.c`, `spawn.c`), `a2dp.c` split into link/SDP/sink/AVRCP/source | refactor | a different structure; not ported (would conflict with every fork change) |
| `make lint`, pinned CI actions, dev container, Windows install docs (WSL2) | CI, docs | the fork has its own CI; Windows Git Bash setup |

## What the open items are

**Core locking and liveness (`335f41a`).** The satellite core (`main.c`) is one state machine that many threads touch under one lock, `core_lock`. The original
found, on its own Echo, ways that lock or the state behind it could wedge the satellite:
- *Numbered reply streams*: a cut (barge-in, "stop", media stop) must drop only the reply it was meant for. In the old code a media stop during a slow fetch left
  a flag set and every later ESPHome reply was silent until a restart.
- *Mic audio through a queue and a sender thread*, so noise reduction and gain run outside the lock and a stalled client cannot make the wake word deaf (the client
  write queue, which the fork now has, covers the writes; this covers the capture thread's own work under the lock).
- Wake word detections and "stop" handed to the capture thread, which owns the ring buffer, instead of being poked from other threads.
- *Child processes with a deadline* (`run_output`: poll 1-2 s, then kill the process group), so a hung helper cannot hold the lock; volume and EQ read at start.
- A bounded TTS queue (1 MiB), the earcon thread on a condition variable, thread creation checked, a failed wake word switch falling back and saying so.

It is not a feature; it is the difference between "usually fine" and "cannot hang". The fork's core has changed a great deal since (second wake word, microWakeWord,
separate volumes, Drop In, light ring), so the patch does not apply as a block. The plan is to port its fixes one at a time, each with the test that fails on the old
code (the first: a media stop during a slow fetch, then an announcement and a reply both play).

**Arbitration limits and bounded peers.** Several Echos on one network exchange broadcasts. The original caps how many candidates, members and pairings one packet can
create, so a hostile or buggy device on the LAN cannot make an Echo allocate or loop without end. Small changes, each worth a review in the fork's own `arb.c`.

**Network hardening of Wyoming and the push port.** An allowlist for who may talk Wyoming, and a back-off on the update port after wrong signatures. The fork's
Wyoming is the secondary protocol and the push port only accepts the owner's key, so the gain is smaller than in the original.

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
| Bluetooth speaker group (search, play on, status, delay), Illuminance | yes | when the feature is on |
| Firmware (update, with an install button) | yes | a text sensor with the version (online updates removed) |
| Online updates (select) | yes | **removed** (GitHub) |
| Last request whispered | yes | **removed** (needed Amazon's download) |
| Timer ringing, Ringing timers, Stop ringing | no | yes |
| Drop In, Drop In with, End Drop In, Drop In result, action `drop_in` | no | yes, kept for now |
| Light ring, Button event, Wake word event, Identify, Restart, Factory reset, Web UI address, Assistant/Sounds/Timer/Call volume | no | yes |
| **AirPlay 2** (switch) | no | yes |
