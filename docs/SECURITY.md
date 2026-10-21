# Security

What protects an Echo running hassmic, and where that ends. Nobody has audited any of it; see the caution in the
[README](../README.md).

## Network

| Direction | What gets through |
|---|---|
| **Out** | Amazon's daemons: local addresses only, plus DNS to the servers DHCP hands out. `otad` and `ace_otad` (firmware updates): never. hassmic itself: anywhere, to fetch replies and music from where Home Assistant or Music Assistant point it. |
| **In, TCP** | 16384–32767 only: 26053 ESPHome, 16700 Wyoming, 28928 Sendspin, 28929 updates, 28931 settings page |
| **In, UDP** | 16384–32767 only (28930: arbitration between Echos, 28932: Drop In audio), and 2330 only while arbitration is in Kiosk Satellite mode (their fixed port) |
| **In, ICMP** | ping (echo request) from private and link-local addresses only; answers to the Echo's own traffic |

Put the Echo on a network without internet as a second layer.

## Links

- **ESPHome**: encrypted like an ESPHome device with `api: encryption` but no key in its YAML. Home Assistant generates
  the key when you add the Echo, sets it over an encrypted connection, and clears it when you delete the device.
  **Until then anyone on the network can connect**, or set a key first (then see
  [When something is wrong](../README.md#when-something-is-wrong)). The key lives in `/data/local/hassmic/state/api_key`.
- **Wyoming**: unencrypted and unauthenticated, like every Wyoming satellite.

## Settings page

Plain HTTP: an Echo has no certificate a browser accepts. So:

- It never shows a secret (API key, Sendspin token, network keys).
- A browser gets in only by a press of the Echo's action button while its login waits; two browsers asking at once are
  both refused.
- Every request after that is signed with a key only that browser and that Echo share (X25519), with a counter against
  replays; so are the answers.
- The one secret the page sends, a Wi-Fi password, is encrypted with that key; the Echo keeps only the network key
  derived from it, as `wpa_supplicant.conf` does.
- Someone who can change traffic on your network (not only read it) could change the page itself, as with any plain
  HTTP page.

The light ring's designs go the other way: hassmic writes them (`state/ring-applied`), and root makes the ring's
animations from them with its own hassmic and writes those to the system partition. Root reads that one file only if
it is a regular file and not a link, at most 8 MB, and takes nothing from it but colours and times; the request
to install is moved into root's own folder before root looks at it, so it cannot be swapped for a link or a pipe
meanwhile, and a design's large upload is read only from an approved browser; the designs can
take at most 3 MB of the partition, and root checks the room before it writes.

## adb

A root shell without authentication: the unlock turns adbd's key check off. Over Wi-Fi it is closed: adbd runs without
its network listener and the firewall drops port 5555. The [ways to open it](GUIDE.md#adb-over-wi-fi) need an
approved browser plus a press of the action button for that one request, a fresh challenge signed with your update key
(a recorded exchange does not work twice), or `ADB_WIFI=1` in root's `hassmic.conf`. Opened with the update key (`scripts/adb-wifi.sh`), the
firewall admits **the address that signed the request and no other** (INPUT rule with `-s <ip>/32`; anything that is not one plain IPv4 address
is refused, not widened); the switch in Home Assistant, the settings page and `ADB_WIFI=1` admit the network.

While it is open, anyone on the network has root. USB always works: physical access is root access anyway. Without
`hassmic.conf` (stock behaviour, or before the install) it is open, as stock leaves it.

## Drop In

A call opens the called Echo's microphone, at once by default. Only a member of the Echo network can ask for one (the
calls travel signed with the network's key), Home Assistant only over its encrypted link, the settings page only from
an approved browser. Each call gets its own key from a fresh key exchange between the two Echos, so a recording stays
closed even to someone who gets the network key later. The audio goes straight between the two Echos and nowhere else.
Do not disturb and the microphone switches refuse calls; "Drop In answers: after the action button" makes every call
wait for a press on the Echo called; "Drop In" off stops calls both ways.

## Arbitration between Echos

An Echo takes the network key only from an Echo Home Assistant vouches for, from Home Assistant over its encrypted API
link, or from the button pairing.

- **Vouched for**: each Echo reports the tag `hassmic_<its public key>` as scanned. The key goes on the network,
  encrypted to the receiver, only between two Echos that each saw Home Assistant record a new scan of the other's tag:
  so only devices you adopted, and only while they are. A user of your Home Assistant could scan such a tag too, from
  the companion app.
- **Action**: where that fails, a member asks Home Assistant to run the newcomer's action
  `esphome.<node>_arbitration_key` (needs "Allow the device to perform Home Assistant actions").
- **Buttons**: a member hands the key only if exactly one Echo asked, from 2 minutes before its own buttons were held.
  Someone on the network who asks as well only makes it fail, and the newcomer's buttons go first.

The key travels encrypted to the receiving Echo, so it is not readable in Home Assistant's states, traces or logbook.
Rounds are authenticated with the key and cannot be replayed. Keys: `state/arb_key`, `state/arbitration`.

**Kiosk Satellite mode** has none of this: its claims carry no key, so any device on the network can silence the Echo
while that mode is on.

## Updates

Installed only when signed with your `secrets/update.key` (pushed from your PC). Root checks
the signature, with the tool and key from the system partition or the installed copy, before anything is unpacked.
Your key also opens adb over Wi-Fi. The Echo fetches no update from the internet.

## Bluetooth

- Keys in `state/ble_bonds` (proxy) and `state/bt_keys` (speaker), under `/data/local/hassmic/`.
- **Hostile peers**: SDP data lengths and L2CAP MTUs are checked, an authentication failure no longer deletes a link key, pairing happens only inside its
  window, audio at a rate the player has no room for is refused, the ACL credit pool is shared with LE.  "Bluetooth phones: AAC" is off by default: AAC goes
  into the firmware's FFmpeg, a big decoder fed by whatever a phone sends; "Bluetooth proxy: secure pairing only" (off by default) turns away devices without LE
  Secure Connections and stops using legacy bonds.  Turning either the weaker way from Home Assistant needs the encrypted connection.
- **Setup from a phone (Improv)**: offered only without a network: to anyone in range on an Echo that is not set up
  (nothing to lose there), only after a press of the action button on one that is. The Wi-Fi password crosses
  Bluetooth unencrypted, as with every Improv device; the Echo keeps only the network key derived from it.

## Root side of updates and the boot

`scripts/system/main.sh` runs as root and shares directories with the daemon, so it never writes through a name the daemon could have turned into a link:
the update's result is written in root's own folder and renamed into `state/ota/` (a link or directory in its place is cleared first), and the mDNS file is made
by the daemon's user in root's folder and renamed in.  The start counter of an update is reset only by a passed self test of the update's own binary (a hung
update falls back, a fall back sticks).  A bundle whose scripts carry CR LF (a Windows checkout) or do not parse is refused; a `hassmic.conf` with CR LF is converted
and one that does not load leaves the firewall up and is not counted against the update.  `tests/boot_test.sh` runs `main.sh` itself against stand-ins under mksh.

Network limits: ESPHome gives a connection 10 s for its first message and makes room when all slots are taken; the push port cuts a connection that trickles
bytes; Sendspin takes the pause command only from an admitted, paired session.

## Factory reset

From Home Assistant only over its encrypted link (once it set a key), from the settings page only from an approved
browser, else by holding the button.
