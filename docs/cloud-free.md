# A cloud-free Echo: Amazon's services removed, not just stopped

Upstream's way of keeping Amazon out is to stop its services at start-up and to block them in the firewall
(`alexa-off.sh`, `lockdown.sh`). Everything stays on the system partition and is started by init at every boot, then
stopped again. This fork goes further, on the Echo Dot 3 (`donut`, Fire OS 6574.1): **the cloud-connected services, their
start-up and their files are taken off the system partition**, and the Echo is tested to still do everything the satellite
does. What is left is Amazon's audio engine and wake word engine, the light ring, the buttons, power and Wi-Fi.

Everything here was measured on one Echo Dot 3 on 2026-10-21 (see [Results](#results)); where something is inferred and not
shown, it says so. The tools are in the repository, the guided setup has a step for it ("Remove Amazon's services"), and the
information needed to repeat it on another firmware or model is below.

## Contents

- [What changes compared with upstream](#what-changes-compared-with-upstream)
- [Results](#results)
- [How to do it](#how-to-do-it)
- [The approach, and why live tests are not enough](#the-approach-and-why-live-tests-are-not-enough)
- [What stays, and why](#what-stays-and-why)
- [What was removed](#what-was-removed)
- [Findings worth knowing](#findings-worth-knowing)
- [What is still on the disk, and why it cannot connect](#what-is-still-on-the-disk-and-why-it-cannot-connect)
- [What you lose](#what-you-lose)
- [Recovery](#recovery)
- [Porting it to another model or firmware](#porting-it-to-another-model-or-firmware)
- [Open](#open)

## What changes compared with upstream

| | upstream | this fork |
|---|---|---|
| Alexa client, cloud services, telemetry, updates | on the system partition, started by init, stopped by `main.sh`/`lockdown.sh` | **not there**: init definitions, executables and libraries moved off `/system`, then deleted |
| Start-up | init starts all of them, `main.sh`/`lockdown.sh` stop the Alexa, update and cloud ones (44 init services still ran) | 28 init services, none of them Amazon's cloud; the scripts have nothing to stop |
| Stop lists in `device.conf` | `ALEXA_SERVICES`, `UPDATE_SERVICES`, `CLOUD_SERVICES`, ... | emptied by the marker `/system/.debloated` |
| Firewall | egress lock needed to keep Amazon's daemons quiet | still there as a second layer; nothing left that it has to catch (drop counter stays 0) |
| `libperf_cpu_info_parser.so` | Amazon's | our 1.7 KB stand-in, because the mixer needs it ([why](#the-mixer-needs-perfmonitord-or-a-stand-in)) |
| New | | `scripts/backup.sh`, `scripts/debloat.sh`, `devices/donut/debloat*.list`, `tools/debloat-analyze.py`, `src/stubs/perf_stub.c`, two wizard steps |

The satellite itself (`hassmic`) is unchanged: it never used what was removed.

## Results

Measured on a Dot 3 running hassmic 2026.10.06, cold boots, Home Assistant connected, before and after.

| | before | after |
|---|---|---|
| init services running | 44 | 27 (+ `avahi-daemon`, which `main.sh` starts) |
| Amazon services running that are cloud-facing | 16 that the live tests stopped one by one: `commsd`, `inloc_service`, `ace_blemesh_service`, `amakit_server`, `halo-*` (3), `tokend`, `dha_service`, `acedropboxd`, `ace_coex_metric`, `neo-*` (2), `perf*` (3) | **0** |
| processes with the permission to open an internet socket | not counted | 5 in group `inet` (`hassmic`, `adbd` (gated), `dhcpcd`, `wpa_supplicant`, `netmgrd`) and the root-owned `avahi-daemon` and `kisd` (no sockets) |
| firewall: packets reaching the final DROP | 0 | 0 |
| `/system` used | 235 268 kB | 139 040 kB with the pass-4 to pass-8 files still in the quarantine (a move frees nothing; `purge` does), 138 756 kB after passes 1-3 were purged |
| `MemAvailable` | 357 MB | 385-388 MB |
| load average | about 7 | about 4-5 |
| files removed | | 67 executables and scripts, about 230 libraries, 3 apps, about 40 init files, and 60 lines of start-up text (`start`/`stop`/`import` lines, data folders, one migration script call) |
| time until the ESPHome port answers | not timed | about 50 s |

"Could open an internet socket" is not a guess: Android's paranoid networking refuses `socket(AF_INET)` to any process outside
group `inet` (3003) and without `CAP_NET_RAW`. On the Echo, run as the mixer's user:
`/system/hassmic/runas audio audio toybox nc -w 2 <host> <port>` → `nc: socket 1 0: Permission denied`; as a user in group
`inet` it connects. The Amazon daemons that stay (`mixer`, `ledcontroller`, `acebuttond`, `aceinputmanager`, `pwrsvcd`, `ace_*`) are
not in that group, so they cannot reach any network, whatever code they have loaded.

## How to do it

**With the guided setup:** after "Install" come "Back up the Echo" and "Remove Amazon's services". The backup asks first (2.2 GB,
about 45 min over Wi-Fi); the removal asks, moves the files, reboots, checks the Echo (processes, microphone), asks you to say the
wake word once, and asks whether to delete the moved files for good (default: no).

**By hand, on an Echo that already runs hassmic** (adb to it; `scripts/adb-wifi.sh <ip>` opens it):

```sh
scripts/backup.sh                    # every partition but userdata + /data as a tar, each checked against sha1sum on the Echo
scripts/debloat.sh -n apply          # shows what would move
scripts/debloat.sh apply             # moves init definitions, executables, libraries, apps into /system/.debloat on the running slot
adb reboot                           # then reopen adb: scripts/adb-wifi.sh <ip>
scripts/debloat.sh verify            # needed daemons up, removed ones not, microphone delivers
#   say the wake word; press the action button, the volume keys, the mute switch
scripts/debloat.sh revert            # (+ reboot) puts everything back, as long as it has not been purged
scripts/debloat.sh purge             # deletes /system/.debloat for good: only the backup brings it back after this
```

`debloat.sh` works only on the active slot's system partition; the other slot stays as it was. It writes `/system/.debloated`,
which `device.conf` reads to empty the stock service lists.

Files:

| file | what |
|---|---|
| `devices/donut/debloat.list` | init files to move (`rc`), service blocks to cut out of shared init files (`block`), start/import lines and data-folder lines to delete (`unstart`, `unimport`, `unline`), our own file in place of a stock one (`install`) |
| `devices/donut/debloat-files.list` | executables, libraries, apps, configs to move (`file`); generated by `tools/debloat-analyze.py`, plus a hand-written tail |
| `devices/donut/prebuilt/libperf_cpu_info_parser.so` | the stand-in library, built from `src/stubs/perf_stub.c` (`make DEVICE=donut`) |
| `scripts/backup.sh` | resumable, verified raw backup |
| `scripts/debloat.sh` | `apply`, `revert`, `status`, `verify`, `purge`; `-n` shows without doing |
| `tools/debloat-analyze.py` | works out the executables and libraries from the unpacked firmware |
| `docs/cloud-free.md` | this |

## The approach, and why live tests are not enough

1. **Look first.** Pull every init file, list the 229 service definitions, and for each: who starts it (`on property:` triggers,
   `class_start`, `start` lines), as which user, with which binary. Boot chain and `ps` of the running Echo next to it. Most of the
   definitions (81) are MediaTek's and point to binaries that are not on this firmware: they do nothing and are left alone.
2. **Stop it live, one at a time** (`stop <name>`), and check after 20 s that the mixer, the ring, the buttons and the Home
   Assistant link still work. This finds the obvious dependencies and costs nothing: a reboot undoes it.
3. **Move the definitions** (not the files) out of init and reboot. A service nobody defines cannot be started by any trigger.
4. **Cold-boot test**, with the wake word and the buttons. This step is not optional. The mixer's dependency on `perfmonitord`
   ([below](#the-mixer-needs-perfmonitord-or-a-stand-in)) passed step 2 (stopped live for minutes, nothing wrong) and
   broke the microphone at the first boot without it: the mixer talks to it when a stream is *opened*, and a live stop leaves the
   connections that were open.
5. **Move the files** (executables, then libraries nothing that stays can reach) into a quarantine folder on the same
   partition, boot again, test again.
6. **Purge** the quarantine. From here on, only the backup restores it.

Never delete first and test later: the quarantine costs 70 MB for a few days and makes every step reversible with `mv`.

## What stays, and why

27 init services after a cold boot; none opens an internet socket except the first five in the list.

| service | what it is | network |
|---|---|---|
| `hassmic`, `hassmic_fw` | the satellite, and the watcher that keeps the egress lock and the adb gate | yes (ours) |
| `adbd` | adb; over Wi-Fi only while `lockdown.sh` has opened it | gated |
| `dhcpcd-wlan0`, `p2p_supplicant` (the running wpa_supplicant), `netmgrd` | Wi-Fi: address, association, Amazon's network manager (local) | yes (needed) |
| `mixer` | Amazon's audio engine with `libasp` (echo cancellation, beamforming) and the Pryon wake word | no |
| `BTSinkPlayer` | Bluetooth sink part of the mixer's path | no |
| `ledcontroller` | the light ring; it also sets `PUFFIN_START`, which is what starts hassmic | no |
| `acebuttond`, `aceinputmanager` | action/volume/mute buttons | no |
| `ace_eventmgr` | the ACE event bus (publish/subscribe over AIPC service 6). Mapped by ledcontroller, acebuttond, aceinputmanager, ace_powerd, ace_thermald, netmgrd, pwrsvcd, mixer, BTSinkPlayer and even hassmic (through libmixerAPI): the hub the button, power and ring daemons talk through | no |
| `ace_powerd` | the power manager: power "resources" with usage tracking, shutdown/reboot events. `mixer` asks it for a resource whenever playback starts or stops (`Mixer_PowerManager:GetResource`), the only client besides itself: the same kind of dependency as `perfmonitord` was, so it stays | no |
| `pwrsvcd` | wakelock bookkeeping and Amazon's "perflocks" (CPU governor and frequency profiles, Wi-Fi power save), 10 clients through `libpwrsvc.so`. It also holds the dummy/USB wakelocks (autosleep stays off), which is why the Echo never suspends. Its Wi-Fi power-save call to the network manager fails with -21 about every two seconds (341 lines in 12 min), because `main.sh` stops `wifisvc` once the link is up: noise, not damage | no |
| `ace_thermald` | thermal policy: reads the 8 thermal zones and sets their trip points (skin 56-61 °C, case 55-60 °C, CPU 90-115 °C) and mitigation steps. No client at all; it is hardware protection, so it stays even though nothing talks to it | no |
| ~~`ace_sensorsd`~~ | **removed** (cold-boot tested 2026-10-21). The ACE sensor service: it registered one sensor, `puffin-als-0` (the ambient light sensor), and its metrics, and nothing connected to it (no process maps `libace_sensors_client`). hassmic reads the light sensor from sysfs itself; after the removal it reads the same 262 lux as before, as hassmic's own user. | |
| `shmd` | the shared-memory broker: binder service `lab126.service.shm`, creates ashmem regions by name, counts references, cleans up when a client dies. `libshm` is mapped by every process on the Echo, hassmic included, so it is a system-wide dependency | no |
| `kisd` | MediaTek's key-installation daemon: reads the key block partitions `kb`/`dkb` (and `/data/key_provisioning`) and installs the device's DRM keys into the TEE at boot; it can also erase a key block on factory provisioning. Root, no sockets, no clients, does its work once per boot. Kept because it touches the keys, not because anything needs it every day | no |
| `rpmb_svc`, `store_seed`, `keystore` | secure storage for the TEE, the random seed, Android's keystore | no |
| `servicemanager`, `dbus`, `logd`, `debuggerd`, `ueventd`, `watchdogd`, `console`, `init` | Android base | no |
| `avahi-daemon` | mDNS; started by `main.sh`, not by init | local |

`kvs_util` (Amazon's key-value store set-up, `init.ace.rc`) is not in the list because it exits after setting
`kvs_init_completed=1`, the property that starts the button, power, thermal and input daemons. Do not remove it.

## What was removed

By group (every service, with its reason, was written down while doing it; the table below is the summary):

| group | services | why it went |
|---|---|---|
| Alexa client and helpers | `puffin` (PuffinApp), `puffinmrmd`, `dacd`, `smarthomed`, `ahe`, `shs`, `quantum` | replaced by hassmic; the smart-home hub and local Alexa |
| Communications, presence, mesh | `commsd`, `inloc_service`, `ace_blemesh_service`, `amakit_server`, `socks5stream`, Sidewalk (`halo-*`, `hub-core`, `psbroker`, `cp_sidewalk`, `halo_logr`) | cloud features; Sidewalk's `halo_logr` is started with the serial number as its uploader id, and `halo_bssid_scan` scans Wi-Fi access points |
| Neo framework | `neo-init`, `neo-service-coordinator` and its apps: credential manager, **Diode**, factory reset | Diode is Amazon's remote diagnostics and command channel (cloud messages, `executeAction`, `deepFactoryReset`, a network-diagnostics job every 5 min) |
| Telemetry | `ace_metricd`, `minerva_service`, `aceusagestatd`, `usagestat_protod`, `acedropboxd`, `logmgr`, `trackerd`, `fmonitor`, `ace_coex_metric`, `UdssCampSvc`, `adepd` | metrics and log collection for Amazon |
| Account and setup | `tokend`, `dha_service`, `provisionerd`, `ffs_provisionee_svc`, `oobed*`, `uxeventd`, `ace_messaging`, `credmgrsvc`, `assetmgrd`, `gadgetsd`, `cmbd` | registration, tokens, setup mode, downloads |
| Updates and clock | `otad`, `ace_otad`, `update_engine`, `sntpd`, `time_update` | firmware updates and NTP to Amazon |
| Performance monitoring | `perfmonitord`, `perfhald`, `perfrecoveryd` + `perfinfo.ko`, `perf_cpu_info.ko` | statistics for Amazon's metrics, a reboot after a week of high memory, a memory cgroup |
| Factory reset | `factory-reset` | the one Amazon daemon left with network permission and cloud-capable libraries loaded; its client was the Neo app. hassmic has its own reset |
| Bluetooth manager, demo | `btmanagerd`, `demo_d3` | hassmic drives the controller itself |
| Start-up text | `exec ... csm2puffin.sh` (a script that is not on the firmware), `start csm2puf_rollback` (a service that does not exist), `mkdir`/`chown` of `/data/avs`, `/data/gadgets`, `/data/cmbd`, `/data/davs`, `/data/wha`, update_engine folders, `import init.perf*.rc` | calls and folders of removed services |

Left alone on purpose: the MediaTek definitions whose binaries are not on the firmware (81 of them). They are dead text: init logs
"could not execve" for the few that are not `disabled`, nothing else. Removing them would mean editing `init.mt8167.rc`, which
holds the 400 `chown`/`chmod` lines that give the drivers their permissions, for no gain.

`init.oobe.puffin.rc` keeps its firewall services (`def_firewall`, `en_p2p_*`): stock's `firewall.sh` is what sets the INPUT and OUTPUT
policy that `lockdown.sh` builds on.

## Findings worth knowing

### The mixer needs perfmonitord, or a stand-in

`mixer` (and `PuffinApp`) import three functions from `libperf_cpu_info_parser.so`: `perfmonitord_initConnection`,
`perfmonitord_dumpPerfCpuInfo`, `perfmonitord_closeConnection`. The first does `aceAipc_connect` to AIPC service 0xb6 (182),
`perfmonitord`'s; the second sends it one message. Without the daemon, the connect waits 20 s for its server, in the mixer's
record thread. Result: `micAsr delivers nothing (status 110)`, no wake word, buttons that start listening and hear nothing.
`lockdown.sh` already said so ("every new micAsr stream makes the mixer connect to it ... each hassmic (re)start was 20 s deaf").
The stock return values are 1 = done, 0 = failed (read from the disassembly). `src/stubs/perf_stub.c` returns 1 and does
nothing, with no dependencies (1740 bytes); `debloat.sh` installs it over the stock library by writing a new file beside it
and renaming (overwriting a library a running process has mapped can crash it). With the stub in place `perfmonitord`,
`perfhald` and `perfrecoveryd` can go; the other three functions of the library are used only by `perfmonitord` itself.

The cheaper alternative was patching three PLT entries in `bin/mixer` (12-byte ARM stubs at 0xe4fc, 0xe508, 0xe538 on this
firmware) to `mov r0,#1; bx lr`. It was not done because `bin/mixer` is hash-pinned in `probe.md5`.

### A library being loaded is not the same as being able to use it

At the time of the audit 17 files left on the disk contained Amazon host names (`libace_connectivity_manager.so` has ten AWS IoT names), and several are
loaded into running daemons as shared-library baggage. They cannot be removed without breaking the daemons that link them. They
are harmless because the daemons are not in group `inet` (see [Results](#results)). The only Amazon daemon that had both the
permission and such libraries was `factory-reset`, and it is gone.

### Live stop tests miss start-time dependencies

See [the approach](#the-approach-and-why-live-tests-are-not-enough), step 4.

### `/` will not go read-only again after replacing a mapped library

`otatool remount ro` answers `Device or resource busy` when a file has been replaced while a process has it mapped (the old
inode is `(deleted)` in `/proc/<pid>/maps`). `debloat.sh install` therefore skips a file that is already identical; after a
real replacement a reboot clears it. A boot always mounts the system partition read-only.

### Other things that look like cloud and are not

- `kisd` is MediaTek's key service (root, no sockets). `rpmb_svc` is secure storage for the TEE.
- `libace_fr`, the `davs` and `ace_kvstore` groups, `factory_reset` are group and library names; none is a network feature.
- `factory_reset`'s `fr_service` (`pre_factory_reset.sh`) stays: disabled, started by nothing.
- The settings page's "Download from Amazon" (hassmic `davs.c`, `dha.c`) needs `fireos-dha`, `tokend` and the DHA libraries and does
  not work any more. Wake words are the ones already on the Echo.

### Tools that were not on the Echo

The Echo has no `sha256sum` (only `sha1sum`, `md5sum`), no `awk`, no `nc` (use `toybox nc`), and its `tail -1` does not do what
it does elsewhere (use `-n`). The scripts here do the heavy parts on the PC.

## What is still on the disk, and why it cannot connect

- shared libraries with Amazon host names, listed above, loaded into local daemons without network permission;
- the stock factory-reset helper script `pre_factory_reset.sh` and its disabled service;
- MediaTek's dead service definitions;
- the other A/B slot, untouched (it still has everything, and nothing runs from it).

If you want the second slot clean too, flash it from the same procedure; it is not needed for the Echo to be offline.

## Installed but not running: what the rest of the disk is

Two further passes done and tested (cold boot, `verify`, the helper programs, a search of every log for the moved names; 2026-10-21): 31 Amazon executables, 68 libraries and 3 init files, then
58 `/etc` entries and 9 more libraries. After them 2076 files are left under `/system` (the quarantine not counted). Sorted by what the running Echo does with them:

| class | files | MB | what it means |
|---|---|---|---|
| running executables | 24 | 3.4 | the services above |
| loaded now (mapped by a running process) | 200 | 51 | in memory; accounted for |
| run on demand by `hassmic` or our scripts | 9 | 0.6 | see the first trap below |
| reachable by name from those | 243 | 25 | a kept file mentions the name (DT_NEEDED, a dlopen string): **an upper bound, not proof of need** |
| executables not reached | 117 | 4.6 | Android/toybox/MediaTek tools, Amazon's local developer tools; none is Amazon cloud code |
| libraries not reached | 47 | 6.2 | the two radio drivers (`.ko`), the Android hardware modules, audio effect plug-ins, platform helpers |
| `etc`, `usr`, `media`, `local`, `hassmic`, ... | 1436 | 6.7+ | see [`/etc`](#etc) below |

`devices/donut/dormant-files.tsv` has every file of the two "not reached" rows and the first pass one by one (class and reason).

**Three traps found while classifying; any removal must respect them**

1. **"Not running" is not "unused".** `hassmic` runs `audio_manager_get_prop` and `audio_manager_set_prop` to read and set the mixer's volumes (`main.c`) and
   `lipc-get-prop`/`lipc-set-prop` for the Bluetooth speaker output (`btout.c`). They were first filed as "optional developer tools" because nothing was running
   them at that moment; the two `liborpheus*` libraries they link were nearly removed with them. The list of programs `hassmic` and our scripts can run is
   built from the sources, the shipped binary and the scripts (16 installed programs), and no program on it may be in a removal set.
2. **Kernel modules** (`wlan_mt76x8_sdio.ko`, `btmtksdio.ko`, the radio) are loaded by init and never appear in a process map.
3. **Android hardware modules** (`lights`, `sensors`, `gralloc`, `audio.*`, `bootctrl`...) are opened by a name libhardware builds at run time; no string names them.
   Also short-lived tools init starts (`kvs_util`, `wifisvc`, `firewall.sh`, `btmac.sh`, `enableswap.sh`, `random_seed.sh`, `dhcpcd-run-hooks`, `amzn_xo_tool`,
   `healthcheck`, `idme`).

**What the second pass removed**: Alexa's skill container and the to-do app, `ace_dioded` (the Diode daemon), the Matter, BLE-mesh, indoor-localization and AMA-kit
services and command lines, `amzn_dha_tool`, `amzn_mmc_utils` (eMMC wear report for Amazon's metrics), `backup_whitelist` (update rollback), `mozartd` (a JSON-over-LIPC
request server for Amazon apps), `wifi_monitor`, `smarthome_utility_puffin` (an Amazon S3 test bucket URL), the Sidewalk hub's remains in `vendor/bin`, and 68 libraries: the Alexa/AVS client stack
(`libACL`, `libAVSSystem`, `libacsdk*`, the `ahe` skills, smart home, accounts and registration) and the Sidewalk hub libraries, plus the attestation HAL `amzn_dha.mt8167.so`
(`libacehal_dha.so`, which only `hassmic`'s Amazon-download code opens, now finds nothing to load: that feature is off anyway).

**Amazon host names still on the disk: 6 files.** Four are loaded into running daemons as dependencies (`libace_connectivity_manager.so` with ten AWS IoT names, `libace_map.so`,
`libace_tlscfg.so`, `libamazon_lws.so`) and can neither be removed nor connect (no `inet` group, see [Results](#results)). `apns-conf.xml` is a false hit (a Brazilian carrier, "Amazônia
Celular"). `dnsmasq.conf` lists Amazon hosts for setup mode's DNS; it belongs to `p2p_dnsmasq.sh` and the `dnsmasq` binary, which only the removed setup mode started: a small candidate for a
next pass. The nine libraries that were only named by other libraries (`libace_amakitsetup`, `libace_arcus_client_api`, `libace_com_amazon_diode`, `libace_ffs_provisioner`, `libace_inloc_manager`,
`libace_messaging_core`, `libace_minerva_core`, `libffs_dssclient_sdk`, `libnative_dev_ota`) were moved in the third pass. Five of them were named by no installed file at all; they only counted as
"reachable" because one library contains the run-time pattern `lib%s_%s.so`, which matches any `libX_Y.so`. That is why the "reachable by name" count is an upper bound.

<a id="etc"></a>
**`/etc` (162 entries, `devices/donut/etc-files.tsv`)**: 104 keep (audio policy, volume tables and tuning, effects, the ring's animations, buttons, power, thermal policy, the certificate store,
init files, the wake word table `locale_to_available_wakewords_map.json`), 58 moved in the third pass (about 1.8 MB: configuration of Amazon software that is gone, such as the Alexa client configuration,
the `ahe` and skill data, smart-home capability definitions, Matter and BLE-mesh data, Amazon endpoint and registration host lists, the update engine's timing and key, a 1 MB retail demo clip). The
analysis behind it, entry by entry: most were named by no file at all; the few that looked alive were false matches (`udss` is the string `com.amazon.udss`, `whad` matched `init.whad_cc.rc`, `ahe` is a
substring of ordinary words, `update_engine` is the service name in `device.conf`, not the directory with the payload key); `uxconfig.json` is named only by `PuffinApp` and the removed `uxeventd`;
`PuffinAppMinervaConfig.json` is named inside `libPuffinUtils`, which every daemon loads, so it was the real test: nothing in any log (9,643 lines of logcat, kernel and `hassmic`) mentions any of the 67
moved files after a cold boot. `adb_cd.sh` (Amazon's adb policy script) stays. "Not named by any file" does not mean unused: the audio policy files are in that group and certainly read, through paths
the libraries build at run time. A file's use cannot be proven from outside; the test is the cold boot and the logs.

## What you lose

- Alexa. (hassmic replaced it before; now `alexa-on.sh` and `MODE=stock-online` cannot bring it back either.)
- "Download from Amazon" on the settings page: new wake words, the sound-detection and whisper models.
- Amazon's firmware updates (they were blocked already).
- The stock factory reset through the button; hassmic's works (action button held 10 s, the HA button, the settings page).
- The automatic reboot after a week of high memory (`perfrecoveryd`). With hassmic at about 30 MB and 385 MB available it has not
  been needed; watch `MemAvailable` over a few weeks.

## The fourth pass: what broke, and how it was done again

Pass 4 (2026-10-21) moved 904 files in one go (232 "orphan" libraries, 592 spoken prompts, 54 UI sounds, 26 Sidewalk leftovers) and the Echo lost its Wi-Fi:
no IP address, no ESPHome port, no adb; the buttons and the wake word still reacted (hassmic and the mixer were up).  Cause, found by reading the init files and
the dynamic dependencies: `libace_netsvc_core.so` (the Wi-Fi manager, linked by `wifisvc`) needs twelve libraries (`libacehal_wifi.so`, `libacehal_network.so`,
`libacehal_wifid.so`, ...) and **all twelve are symbolic links to one file, `libacehal_network_all.so`**, which the pass had moved.  The analysis walked
regular files only (`find -type f`, `os.walk` with `isfile`), so it never saw a link and called the target an orphan.

Recovery needed the USB wires (test pads TM3 GND, TM4 D-, TM5 D+; `devices/donut/README.md`), TWRP (Volume Up while plugging in power; the Echo shows up as
`18d1:4ee2`, which a Mac may hold back until "Allow accessories to connect" is set to Always; a Linux PC just works) and `scripts/debloat-twrp.sh`, which mounts the
active slot and puts everything in `/system/.debloat` back.  Wi-Fi and Home Assistant came back with the next boot.

Lessons, all in the tools now:

- A symbolic link is a use of its target.  `tools/debloat-analyze.py` keeps every link target, and `scripts/debloat.sh` builds the list of link targets on the Echo
  and answers `KEPT <path> (a symbolic link points at it)` instead of moving one.  `tools/system-manifest.py` gives every link a row of its own.
- A group is tested before the next one: a cold boot, `scripts/debloat.sh verify`, then the wake word, the buttons and Home Assistant.  With the USB wires on the board a mistake costs minutes; without them it is a dead Echo.
- Libraries are not removed because their name looks harmless: the 51 with `network`, `wifi`, `audio`, `power`, `led`, `ota`, `device_info` or `button` in the name were
  read and checked by hand (who names them, who maps them, which prefixes are completed at run time) before they went, in three groups.

What was then applied, each with a cold boot and `verify` (lists beside `debloat.list`; `scripts/debloat.sh apply` reads all of them, a list named on the command line stands alone):

| list | files | what |
|---|---|---|
| `debloat-pass4-1-sounds` | 646 | spoken Amazon prompts and the UI sounds hassmic does not play |
| `debloat-pass4-2-vendor` | 63 | Sidewalk and A-GPS leftovers under `vendor/` |
| `debloat-pass4-3-libs-safe` | 143 | libraries nothing names or links |
| `debloat-pass4-4a-libs-tests-ota` | 33 | test, command line and OTA client libraries |
| `debloat-pass5a-ota-metrics-cli` / `5b-network` / `5c-audio-wake-hal` | 8 / 5 / 5 | the 18 libraries held back: named by no file, mapped by no process, no run-time prefix |
| `debloat-pass6a-devtools` / `6b-boot-oneshots` | 21 / 5 | VPN (racoon, mtpd, pppd), bug reports, Android framework helpers, OTA patchers, SoftHSM and Bluetooth command line tools; `healthcheck` and `amzn_xo_tool`, which init started once per boot |
| `debloat-pass7-dead-services` | 5 edits | 32 service definitions whose program is not in the image (init logged "cannot find ..." for each at every boot), and the `import` lines of init files that do not exist |
| `debloat-pass8-bmd` | 2 | two binary tables the manifest could not explain |

Kept on purpose after a check, because they only look removable: `libacehal_network_all.so` and its links, `p2p_supplicant` (it is the Wi-Fi supplicant: the image starts the P2P-capable
variant), `dnsmasq` / `hostapd` (the stock firewall script and the supplicant name them), `kvs_util` (its `kvs_init_completed` starts the button, power, thermal and input daemons), `enableswap`,
`update_verifier` (marks the A/B slot successful), `idme`, `kisd` and `rpmb_svc` (keys).

What is left, file by file and service by service, is in [`devices/donut/SYSTEM.md`](../devices/donut/SYSTEM.md) and `devices/donut/system-manifest.tsv` (1452 entries: 1291 required, 54 by-name, 107 platform, none unknown).

## Recovery

| the Echo | how to bring it back |
|---|---|
| runs, something is wrong | `scripts/debloat.sh revert`, reboot (until you `purge`) |
| runs, quarantine purged | restore the partition from the backup (TWRP or `dd` over adb; `RESTORE.md` in the backup folder) |
| does not start, quarantine not purged | USB (wires on the test pads, see `devices/donut/README.md`): Volume Up while plugging in power (white ring), then `bash scripts/debloat-twrp.sh` puts the quarantine back (add `--reboot`); on a Mac allow accessories first, or use a Linux PC |
| does not start, quarantine purged | USB as above, then restore `system_a.img` from the backup |
| bootloader gone | the bootrom exploit (kamakiri), a Linux PC, then the firmware as for a new Echo |

The backup folder holds `persist`, `tee1`, `tee2`, `kb` and `dkb`: the Echo's own keys. They cannot be made again. Keep it, and keep it
out of git (`device-logs/` is ignored).

adb over Wi-Fi depends on `wpa_supplicant`, `dhcpcd`, `hassmic_fw` and `lockdown.sh`, none of which is touched. If a change ever breaks
Wi-Fi, only USB is left, so solder the USB wires before experimenting.

## Porting it to another model or firmware

1. Unpack the firmware (`devices/<codename>/README.md`) and pull the init files; run `tools/debloat-analyze.py` for the
   executables and libraries, after writing `debloat.list` for the services. Start from the donut list: a service's name tells most
   of the story (`ace_*`, `halo-*`, `puffin*`, `neo-*`).
2. For each service the list removes: is it started by a trigger a kept daemon sets (check the `on property:` lines)?
   Does a kept binary import its client library (`readelf --dyn-syms`)? That is how `perfmonitord` was found.
3. Do it in the order above, on the running slot, with a backup, and cold-boot after every group.
4. Put the model's differences in `devices/<codename>/` (lists, `PERF_STUB` in `device.mk`, a prebuilt stub if its mixer links the
   same library). No `#ifdef`s in shared code.

## Open

- The seven ACE/MediaTek daemons above were identified from their libraries, who maps their client libraries and their logs (2026-10-21), not removed one by one.
  `ace_sensorsd` had no client and was removed and tested; `ace_thermald` and `kisd` have no client either but protect the hardware and the keys.
- A dead-man switch is not built yet: after `apply`, a flag that makes `main.sh` put the quarantine back and reboot when the Echo has no IP address after a few minutes (cleared by `verify`).  Until then the USB wires are the safety net.
- `light blue flashes until Home Assistant is connected` and AirPlay 2 are in PLAN.md.
- A tidy-up of hassmic's own dead code for what is gone (`davs.c`, `dha.c`, the `alexa-*` scripts) has not been done.
