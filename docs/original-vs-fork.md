# The original repo and this fork

Compared on 2026-10-22: `original/main` (github.com/0x0520/echo-dot-assist, last commit `4de5c35` of 2026-10-06, the build that ran on the
Echo until the evening of 2026-10-21) and this fork (`cloud-free`). Both grew from the same commit, `c7794a4` of 2026-10-05
("artifacts.sh: deregister only once the Echo is a satellite again"); since then the original has 50 commits of its own, the fork 99
(plus the cloud-free work).

The Echo was installed from the original (Debian, 2026-10-17). On 2026-10-21 the fork's build was pushed to it by mistake of the line,
which is why entities were gone. This page says what each side has so nothing is lost by choosing one.

## Only in the original (what the fork lacks)

| feature | where | state in the fork |
|---|---|---|
| **Offline alarm clock**: three alarms set from Home Assistant, ring on the Echo without Home Assistant, repeat, snooze, stop | `alarms.c`, `alarmtime.c`, `keyfile.c`; entities "Alarm", "Alarm ringing", "Next alarm", "Snooze alarm", "Stop alarm" | being ported (see below) |
| **Task manager**: CPU, memory, load, top processes, last kill; kill through root | `taskmgr.c`, `scripts/top.sh`; entities "Memory used/available", "Load average", "Top processes", "Last kill" | being ported |
| Bluetooth AAC switch (off by default) | `a2dp_codecs.c`, entity "Bluetooth AAC" | the fork always offers AAC; switch being ported |
| Bluetooth proxy: secure pairing only (off by default) | `ble.c`, entity "Bluetooth proxy: secure pairing only" | being ported |
| "Wi-Fi setup over Bluetooth" switch | `improv.c` | the fork's Improv is automatic (offline 20 s / button), no switch |
| Alexa on without a reboot | `scripts/alexa-on.sh` | not wanted any more (cloud-free) |
| Network hardening: adb for the signer only, Wyoming allowlist, push-port back-off, arbitration limits, bounded network peers, Sendspin/Bluetooth hardening against hostile peers, core locking and liveness fixes | many files, `fix:` commits of 2026-10-05 | **to review and port**, not features but safety |
| Downgrade protection for release-signed bundles | `ota.c`, `main.sh` | not needed: the fork trusts only the owner's key |
| `main.c` split into modules (`core_int.h`, `mic.c`, `playback.c`, `outq.c`, `wakedet.c`, `wakewords.c`, `spawn.c`), `a2dp.c` split into link/SDP/sink/AVRCP/source | refactor | a different structure; not ported (would conflict with every fork change) |
| `make lint`, pinned CI actions, dev container, Windows install docs (WSL2) | CI, docs | the fork has its own CI; Windows Git Bash setup |

## Only in the fork (what the original lacks)

- **Settings page** on port 28931: login by the action button, settings by name, export/import and presets, copying settings and models between Echos,
  the network-wide login, a log viewer, rename, Identify, Wi-Fi switch; light ring designs and theme packs.
- **Wake word**: microWakeWord and Home Assistant as engines, a second wake word at once, "Wake word" event entity, no freeze after 18.6 h, "<wake word>, stop" pauses music.
- **Arbitration** with a learned window, Home Assistant tags for the key handoff, Kiosk Satellite mode, join without "perform actions".
- **Drop In** (calls between Echos, with a voice blueprint) and its tests against a real Home Assistant.
- **Timers**: ringing state, "Stop ringing", "any request stops a ring", their own volume; **separate volumes** for music/replies/sounds/timers/calls.
- **Light ring** as a Home Assistant light, the listening ring turned towards the talker, the Button event entity, speaker gain, wake sound removal.
- **Media over HTTPS**, Sendspin re-announce, state files that survive a hard reset, time zone from Home Assistant, a time on every log line, new-IP handling.
- **Setup**: guided setup for the Dot 3, Windows (Git Bash) support, and since 2026-10-21 the **cloud-free** work (`docs/cloud-free.md`): Amazon's services removed, the Amazon download, whisper detection and online updates removed.
- Tests: `tests/run.py` runs everything in network namespaces, in CI, a real-Home-Assistant job.

## Entities in Home Assistant

| entity | original `4de5c35` | fork before 2026-10-22 | fork now |
|---|---|---|---|
| Media Player, Mute, Do not disturb, Wake sound, LED brightness / auto, Equalizer x3, Bluetooth pairing, Sendspin token | yes | yes | yes |
| Mic level, Noise reduction | yes | page only | **yes again** |
| Bluetooth announcements, announcement language | yes | page only | **yes again** |
| Debug access (adb over Wi-Fi) | yes | page only | **yes again** |
| Join arbitration network, Arbitration peers | yes | page only (peers when on) | **yes again** (switch always, peers when on) |
| Music Assistant without pairing | yes | page only | **yes again** |
| Sound detection switch, Sound (event) | yes | page only, event when on | **yes again** (switch always, event when on) |
| Wi-Fi motion switch, sensor, sensitivity | yes | sensor and sensitivity when on | **yes again** (switch always) |
| SoC temperature, CPU usage | yes | page only | **yes again** (diagnostic, disabled by default in Home Assistant) |
| Bluetooth speaker group (search, play on, status, delay) | yes | when on | when on |
| Illuminance | yes | when on | when on |
| Alarm 1-3, Alarm ringing, Next alarm, Snooze alarm, Stop alarm | yes | **no** | being ported |
| Memory used / available, Load average, Top processes, Last kill | yes | no | being ported |
| Bluetooth AAC, Bluetooth proxy: secure pairing only, Wi-Fi setup over Bluetooth | yes | no | being ported |
| Firmware (update, with an install button) | yes | yes | a text sensor with the version (online updates removed) |
| Online updates (select) | yes | page only | **removed** (GitHub) |
| Last request whispered | yes | when a model was there | **removed** (needed Amazon's download) |
| Timer ringing, Ringing timers, Stop ringing | no | yes | yes |
| Drop In, Drop In with, End Drop In, Drop In result, action `drop_in` | no | yes | yes, kept for now |
| Light ring, Button event, Wake word event, Identify, Restart, Factory reset, Web UI address, Assistant/Sounds/Timer/Call volume | no | yes | yes |
