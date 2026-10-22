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
| shairport-sync `b991d91` | `devices/donut/prebuilt/airplay/` | AirPlay 2 receiver, FFmpeg decoders, output to the pipe; two patches (`tools/airplay/patches`): (1) the "external-avahi" mDNS backend writes the stock avahi-daemon's service files (`airplay-raop.service`, `airplay-ap2.service` beside hassmic's own) with the text records AirPlay 2 needs |
| nqptp 1.2.9 | same | the PTP clock (UDP 319, 320; shared memory in `/dev/shm`); root's `main.sh` (`airplay_clock`, netwatch's 10 s loop) runs it only while `state/config` says `airplay=on` |
| `lockdown.sh` | root | admits UDP 319 and 320, over IPv4 and IPv6 (the Mac sends its clock from its IPv6 address), from private and link-local ranges, only while `airplay=on` (`airplay_state`, like Kiosk Satellite's port); `check` catches them left open |

Ports: RTSP 28933, and 28940-28979 for everything shairport-sync opens per session (event, data, control and audio ports, TCP and UDP), all in
the range the firewall admits already. Stock shairport-sync binds those as "any port", i.e. 40000-60000 (51722, 47902 seen): patch 2
(`0002-ports-in-range.patch`) takes them from `udp_port_base`'s range, one after another.
Name: the Echo's name ("Echo Dot 3 5695c4"). **Volume**: the sender's slider is the volume of AirPlay, over the Echo's whole range: 100 % on the phone is the Echo at its loudest
(shairport-sync applies the slider in software, -30..0 dB). The Echo's own volume (buttons, Home Assistant) does **not** scale AirPlay while it plays on the Echo's speaker;
it is the volume of the replies, sounds, timers and calls. How: while AirPlay plays, `main.c` `airplay_full()` counts it as a source at 100, so the mixer runs at the top and
everything else is turned down in our PCM from there by the Echo's volume curve, exactly as with "separate volumes" (`mix_apply`); AirPlay's music gain stays 1. It stops
counting when AirPlay stops, and with a Bluetooth speaker as the output (its own volume rules there). "AirPlay gain" (0 to +12 dB, 0 by default, with a tanh limiter) is an extra
boost for a quiet source.

The configuration is generated (`state/airplay.conf`). Two things in it matter and cost an evening each: `output_format`/`output_rate` are read from the
`pipe` section, not `general` (from `general` they are ignored and AirPlay 2 gives S32 at 48 kHz: static), and the reader must keep the bytes of a
frame a read ended in the middle of (dropping them shifts every later sample). Log verbosity 2 stays on: its lines go to `boot.log`.

## Why glibc, static

The firmware's C library (bionic) has no `pthread_cancel`, which shairport-sync uses throughout, and no `shm_open`. A statically linked
glibc binary needs no loader and runs on the Echo's kernel as it is (`tools/airplay/build.sh`; Debian's `gcc-arm-linux-gnueabihf`,
hard-float ARMv7 with VFPv3-D16, which the MT8167 has). `getaddrinfo` and `getpwnam` in a static glibc want the shared libraries at run time;
neither is used for anything that matters here (peers are addresses, no user lookups). `tools/airplay/cross-libs.sh` (`GNU=1`) builds the libraries
(OpenSSL 3.0, libsodium, libgpg-error + libgcrypt, libplist, libconfig, popt, libuuid and a minimal FFmpeg 6.1: AAC and ALAC decoders).
Two build snags are in the scripts' comments: libgpg-error has no lock description for the hard-float triplet (the generated one
has a 4-byte lock and the program stops at start with "gpgrt fatal: sizeof lock obj"), and libgcrypt and OpenSSL both define `gf_mul`.

## Delay

AirPlay 2 itself keeps every speaker on one clock (PTP, nqptp) and every sender tells the receivers when each sample is due, so nothing is
tuned per song. What the receiver has to know is how long its own output takes, and hassmic works that out (`airplay.c` `total_ms`):
the mixer queue kept near 110 ms (writes wait for it to fall under 120 ms, as Sendspin's), the FIFO between shairport-sync and hassmic
cut to one page (4 KB, 23 ms; the default 64 KB held 370 ms and was the biggest error of the first version), the mixer, DAC and amp
(70 ms, `src/tools/latency.c`, as Sendspin) and, with a Bluetooth speaker, its buffer (`btout_latency_us`). shairport-sync gets the sum as
`audio_backend_latency_offset_in_seconds`, i.e. it hands the audio out that much early. The setting "AirPlay delay" is only a trim on top
(0 by default); it, or a Bluetooth speaker coming or going, restarts the receiver at the next pause.

## Home Assistant's media player

The Echo's media player entity says *playing* while AirPlay plays (`core_airplay_changed` -> `esphome_media_changed`), and advertises `supports_pause`, so Home Assistant's
voice commands ("stop the music", "pause") and automations find it. Its pause and stop commands drop the AirPlay sender (`airplay_yield`: the phone shows
"disconnected"; AirPlay has no pause that a receiver can ask for) and the entity goes idle at once. The entity itself carries no title, artist or cover art (ESPHome's media player has no field for them). Title, artist and album come from shairport-sync's metadata pipe (`apmeta.c`
parses it; the cover art is not asked for) and are three text sensors beside it, "AirPlay title", "AirPlay artist" and "AirPlay album", while AirPlay is on and empty when nothing plays (not every sender
gives them: Music and Spotify do, a browser tab may not). The sensors cannot be put *into* the media player entity: ESPHome's protocol has no field for them, and Home Assistant's "universal" media player does not help (its title, artist and album
come from its child player only, never from sensors: `media_title` etc. in its `attributes` are ignored). A dashboard card shows them under the player instead (the ids are one Echo's, a room
called Schlafzimmer; yours are in Developer tools -> States, filter `media_player.echo` and `airplay`):

```yaml
type: vertical-stack
cards:
  - type: media-control
    entity: media_player.echo_dot_3_media_player
  - type: conditional
    conditions:
      - condition: state
        entity: media_player.echo_dot_3_media_player
        state: playing
    card:
      type: markdown
      content: >
        **{{ states('sensor.schlafzimmer_echo_dot_3_d13244_airplay_title') }}**

        {{ states('sensor.schlafzimmer_echo_dot_3_d13244_airplay_artist') }}
        · {{ states('sensor.schlafzimmer_echo_dot_3_d13244_airplay_album') }}
```

## Security

Anyone on the network can play to an AirPlay speaker, as with every one; there is no pairing here (shairport-sync's AirPlay 2 uses
transient pairing). The receiver runs as the daemon's user, the clock as root: nqptp only reads UDP and writes shared memory.
Neither reaches the internet (firewall: the daemon's real group may reach local addresses and what hassmic fetches; root's nqptp
answers peers only). Off: no process, no ports, no mDNS records.

## Measured (2026-10-22, Echo Dot 3, AirPlay from a Mac)

Idle with AirPlay on: nqptp 1 % + shairport-sync 0.7 % + the reader thread 1 % of one core (the Echo has four), 3 MB RSS, 6 MB less free RAM. Playing: shairport-sync 7.5 %,
the mixer 75 -> 100 %, hassmic 75 -> 45 % (not understood), total load unchanged at 55-60 %, 11 MB RSS. Wake word and voice replies unaffected. The two programs are
4.3 MB, stored twice (factory copy and update slot).

## Not done

No pairing (open to the LAN), no title, state or cover art in the media-player entity (the pipe output has no metadata), not tried with an iPhone or in a
group with other AirPlay 2 speakers, no live latency report from the output (the delay is the sum of fixed numbers).

## Tests

`tests/fake_ha_airplay.py` (`tests/fake_shairport.py` plays the receiver's part: reads the configuration, writes the records, a tone into the pipe):
off by default, on from Home Assistant's link, configuration and ports, records, audio into the music stream, a gap, "stop", off.
`tests/lockdown_test.sh`: the two ports. A real sender is needed for the rest (pyatv cannot: it times with NTP, AirPlay 2 receivers want PTP): tried by hand with macOS, which is where the ports, IPv6 and format snags above showed.
