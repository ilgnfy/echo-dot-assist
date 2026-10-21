# What is left on the Echo Dot 3 after the cloud removal

State of 2026-10-21, the running slot after `scripts/debloat.sh apply` (every list of `devices/donut/debloat*.list`) and a cold boot.
Every file and symbolic link is in [`system-manifest.tsv`](system-manifest.tsv) with its group, whether it is needed and why;
this page explains the groups and the services behind them.  How and why the rest was removed: [docs/cloud-free.md](../../docs/cloud-free.md).

| | |
|---|---|
| entries under `/system` | 1452 (1291 files and 161 symbolic links), 79 MB of file content |
| `/system` used (df) | 87 652 kB after the final `purge` (stock: 235 268 kB); 912 MB free |
| need | 1291 **required** (a running service, a file such a service loads, or something hassmic, init or our scripts use), 54 **by-name** (not loaded now, but a live file names it: kept until shown otherwise), 107 **platform** (local Android / MediaTek tools and data; no Amazon cloud code) |
| network | listening: hassmic (TCP 26053, 28928, 28929, UDP 28930), avahi (mDNS 5353), dhcpcd (68), adbd while opened; connected: Home Assistant.  Nothing talks to the internet |

A symbolic link is a use of its target: `find -type f` never shows one, and pass 4 of the cleanup moved the target of twelve Wi-Fi HAL links
(`libacehal_wifi*.so` -> `libacehal_network_all.so`) and the Echo lost its Wi-Fi.  The manifest has a row for every link, and `debloat.sh` refuses to move a link target.

## How it starts

1. **Kernel and init** (`/init`, `/init.rc`, `/init.mt8167.rc`, `/init.mt8516.rc`, `/init.aosp.rc`, ... and `etc/init/*.rc`): mount, load the Wi-Fi / Bluetooth modules (`lib/modules`), start the core services below.
2. **`kvs_util`** sets up Amazon's local key-value store and sets `kvs_init_completed=1`; that property starts `acebuttond`, `acepowerd`, `acethermald` and `aceinputmanager`.
3. **`ledcontroller`** (the light ring) comes up and sets `PUFFIN_START`; with `audio_mixer.init` that starts **`hassmic`** through `hassmic.rc` (`/system/hassmic/boot.sh` -> `main.sh`), which also runs the egress firewall (`lockdown.sh`) and keeps Wi-Fi, mDNS and updates in order.
4. **`mixer`** is Amazon's audio engine: microphone capture, echo cancellation and beamforming (`libasp`), the wake word engine (`libpryon`), playback.  hassmic is its client.

## Services running now (`getprop init.svc.*`)

| service | program | what it does, why it stays |
|---|---|---|
| `hassmic`, `hassmic_fw` | `/system/hassmic/...` | the voice satellite (ESPHome API, settings page, music, Drop In), and its firewall/updater root side |
| `mixer` | `bin/mixer` | audio front end + wake word; the satellite's whole audio path |
| `ledcontroller` | `bin/ledcontroller` | light ring animations; also sets `PUFFIN_START` |
| `acebuttond` | `bin/acebuttond` | action, volume and mute buttons (hassmic reads the same keys) |
| `aceinputmanager` | `bin/aceinputmanager` | routes key events to the event bus |
| `ace_eventmgr` | `bin/ace_eventmgr` | the ACE event bus (publish/subscribe): hub of button, power, thermal and ring daemons |
| `acepowerd` | `bin/ace_powerd` | power manager; the mixer asks it for a power resource when playback starts and stops |
| `acethermald` | `bin/ace_thermald` | thermal policy: eight zones, trip points, mitigation (protects the amplifier and SoC) |
| `pwrsvcd` | `bin/pwrsvcd` | wakelocks and CPU / Wi-Fi power profiles; keeps the Echo from suspending |
| `BTSinkPlayer` | `bin/BTSinkPlayer` | Bluetooth audio sink part of the mixer's path (A2DP in) |
| `dbus` | `bin/dbus-daemon` | system D-Bus (avahi and the Bluetooth stack use it) |
| `servicemanager` | `bin/servicemanager` | Android binder service registry (keystore, vold and the HALs register here) |
| `keystore` | `bin/keystore` | Android keystore daemon; DHA / attestation for "Download from Amazon" |
| `kisd`, `rpmb_svc` | `vendor/bin/kisd`, `rpmb_svc` | MediaTek key installation and replay-protected memory (TEE storage) services under the keystore and the device attestation HAL.  Not touched: the page's Amazon download needs the attestation key |
| `shmd` | `bin/shmd` | ACE shared-memory daemon (local IPC buffers of the audio and event daemons) |
| `netmgrd` | `bin/netmgrd` | Amazon's local network manager (netsvc libraries; reports link state, never leaves the LAN) |
| `p2p_supplicant` | `bin/wpa_supplicant -iwlan0 ... -ip2p0` | **the** Wi-Fi supplicant (the image starts the P2P-capable variant; `wpa_supplicant` is the same binary) |
| `dhcpcd-wlan0` | `bin/dhcpcd` | DHCP client for Wi-Fi |
| `store_seed` | `bin/random_seed.sh` | saves the kernel's random seed |
| `logd`, `debuggerd`, `console` | Android | log buffer, crash dumps, boot console |
| `adbd` | Android | adb (USB always; Wi-Fi only when opened) |
| `ueventd`, `watchdogd` | `/init`, `/sbin/watchdogd` | device nodes, hardware watchdog (reboots a hung Echo) |
| avahi | `bin/avahi-daemon` | mDNS announcements so Home Assistant finds the Echo (started by `main.sh`, not by init) |

Stopped one-shots still defined in init (run once or on demand): `kvs_util`, `enableswap` (zram swap), `set_memtotal`, `btmac` (Bluetooth MAC), `nvram_daemon` and `msensord`
(MediaTek radio / sensor helpers, started by drivers), `vold`, `wifisvc` (Amazon's Wi-Fi service; the supplicant and `wifi.sh` do the work), `load_seed`, `logd-reinit*`, `boot2_anim_start`, `fosflags`, `def_firewall`.

## Groups of files (`system-manifest.tsv`, column "group")

| group | files | size | what it is, why it is here |
|---|---|---|---|
| audio & wake word | 21 | 25.5 MB | `libpryon.so` (the wake word engine, its `libfst*` helpers), `libasp` (echo cancellation, beamforming), the mixer's audio algorithm libraries.  The reason the Echo is worth keeping |
| Android base & system libraries | 125 | 23.1 MB | bionic-level libraries (libc++, libcutils, libbinder, libmedia, EGL stubs, ...) the mixer and the daemons link |
| services | 155 | 4.1 MB | the programs in the table above, plus `toybox` applet symbolic links (`ls`, `cp`, `grep` ... all one binary) |
| libraries | 35 | 3.2 MB | kept by name: libraries a live file names but nothing links (the Alexa-hybrid `libfst*`, ASR helpers that libpryon loads with `dlopen`) |
| hardware modules | 20 | 3.2 MB | Android HALs (`lib/hw/audio.*.default.so`, `bluetooth.default.so`, `power`, `thermal`, `keystore`, `gralloc`, `bootctrl`): opened by name through libhardware |
| platform tools | 64 | 2.6 MB | local debug and IPC tools (`aipc-*`, `lipc-*`, `aceipc-*`, `tinymix`, `tinyplay`, `iw`, `ip`, ...); hassmic and `main.sh` call some, the rest are harmless |
| kernel modules | 3 | 2.5 MB | `wlan_mt76x8_sdio.ko`, `btmtksdio.ko` and the prealloc module: the Wi-Fi / Bluetooth drivers (MT7668) |
| etc: audio, system, ring, buttons | 92 | 2.1 MB | configuration of the audio policy and volume curves, crossover tuning, the ring, the buttons, power |
| wake word & sound models | 24 | 1.9 MB | the stock wake word model (Alexa) and the sound detection model that hassmic's optional features load |
| init & tools | 62 | 1.9 MB | helper scripts and programs init, hassmic and our scripts call (`firewall.sh`, `iptables`, `kvs_util`, `ledctrl`, `idme`, `audio_manager_*_prop`, ...) |
| Amazon ACE platform libraries | 46 | 1.5 MB | the local platform layer under the daemons (AIPC, event bus, power, thermal, input, KV store, Wi-Fi manager).  No cloud client among them |
| ours | 18 | 1.5 MB | `/system/hassmic/` (the factory copy of the satellite and its scripts) and the `.debloated` marker |
| vendor: audio front end | 35 | 1.4 MB | `libasp` / Pryon configuration: AEC, beamforming, EQ, throttling per product |
| etc: light ring | 266 | 1.0 MB | ring animations (`led-resources`) plus the layer config hassmic extends |
| etc: certificates | 333 | 1.0 MB | CA store (`cacerts`, `cacerts-sha1` links): hassmic's HTTPS media fetches and updates verify against it |
| UI sounds (earcons) | 13 | 0.8 MB | the sounds hassmic plays (wake, touch, mute, volume, Bluetooth, Drop In ringtones) |
| vendor: firmware | 6 | 0.8 MB | Wi-Fi and Bluetooth chip firmware (MT7668), loaded by the drivers |
| libraries of removed services | 13 | 0.5 MB | despite the name: `libacehal_network_all.so` and its twelve link names, which `libace_netsvc_core.so` (the Wi-Fi manager) needs |
| system data | 1 | 0.4 MB | `tzdata`, bionic's time zone database (`tz.c`, `main.sh tz_set`) |
| audio effects | 5 | 0.2 MB | Android audio effect plug-ins named in `audio_effects.conf` (platform) |
| etc: input / permissions / init / network, audio, thermal / properties | 36 / 16 / 19 / 13 / 1 | < 0.1 MB each | key layouts for the button device, permission XML read by dbus / mixer / wpa_supplicant, init service definitions, dhcpcd / bluetooth / thermal policy config, `build.prop` |
| vendor: MediaTek leftovers / unused network / unused telephony / audio & platform config | 7 / 6 / 3 / 7 | tiny | build flags, thermal blob, factory menu, DHCPv6 and router-advertisement config (services off), emergency call lists (no modem), audio HAL routing XML |
| links | 6 | 0 | the wake word locale folders that point at others (`mul-021` -> `en-US`) |

## Left over, local, and not needed

These stay only because removing them gains nothing (a few MB at most) and each was judged against the lists; they are `platform` in the manifest:
Wi-Fi Direct and access point tools (`hostapd`, `dnsmasq`, `p2p_dnsmasq.sh`, the P2P part of the supplicant: the running supplicant is the P2P-capable one), DHCPv6 and `radvd`, `kvs_util` (needed at boot), `idme` (hassmic reads the identity memory),
`update_verifier` and the rollback init (it marks the slot successful for the A/B scheme), Android framework helpers (`pm`, `svc`, `cmd`, `dumpsys`), MediaTek factory and thermal blobs.
A fifth pass may take some of them; nothing here connects to Amazon.

## Keeping this current

`scripts/debloat.sh verify` checks the Echo after a change.  To rebuild the manifest from a running Echo:

```sh
adb shell 'find /system -path /system/.debloat -prune -o \( -type f -o -type l \) -print | while read -r f; do
  if [ -L "$f" ]; then echo "l|$f|$(readlink "$f")"; else echo "f|$f|$(stat -c %s "$f")"; fi; done' | tr -d '\r' | grep -E '^[fl]\|' > live.txt
python3 tools/system-manifest.py live.txt devices/donut/system-manifest.tsv devices/donut/system-manifest.tsv
```

New files, which the tool does not know, come out as `unknown`: read them (strings, who loads them, what starts them) and give them a group, a need and a reason before the next removal.
