# ESP32-C3 Neato D5 MQTT Controller

ESP32-C3 SuperMini based controller for Neato D5 robot vacuum, enabling Home Assistant integration via MQTT.

## Features
- MQTT communication with Home Assistant
- FreeRTOS-based task management
- Vacuum control and monitoring, over MQTT and a password-protected web interface
- OTA updates over HTTPS
- WiFi configuration, with a fallback access point for recovery

**The vacuum itself is simulated.** `main/vacuum.cpp` is an in-memory state
machine — it publishes plausible states and a draining battery, and nothing in
`main/` yet speaks the Neato protocol documented under `docs/`. The serial bridge
is wired to the robot and works; driving the robot with it is the next step.

## Hardware
- ESP32-C3 SuperMini
- Neato D5 Robot Vacuum

The robot's debug port is `RX | 3.3V | TX | GND`, wired crossover to the ESP
(robot RX to ESP TX, robot TX to ESP RX) and it supplies 3.3V to power the ESP.
The link runs at 115200 baud, matching the serial terminal's default.

## Recovery (no serial access)

Once the board is inside the robot, the network is the only way in, so it is
built not to need the USB port:

- **Reconnection never gives up.** If the access point disappears, the station
  retries indefinitely with a backoff capped at one minute, so a router reboot
  or outage heals by itself.
- **A fallback access point** comes up when the station cannot connect, or when
  no credentials are stored at all. Join it and browse to `http://neato.setup`
  to reach the same web interface and re-provision. It shuts down again once the
  station reconnects, so it is only exposed while it is needed.
- **A factory reset pin** on **GPIO10**: hold it to ground for 5 seconds to
  erase all stored configuration and restart, which brings the access point
  back up. The pin has an internal pull-up, so leaving it unconnected is safe,
  and the LED blinks rapidly while the hold is counting down — release before
  5 seconds to cancel. This is the way back if even the access point cannot be
  reached, for instance because its password was set and then forgotten.

  **It clears configuration, not firmware.** The reset erases the NVS partition
  — WiFi credentials, the interface password, the access point passphrase and
  the broker URI — and returns the device to first-run setup. The `ota_0` and
  `ota_1` app slots and `otadata` are untouched, so whatever image is installed
  is what runs afterwards. Recovering from a bad image means reflashing over
  USB.
- **The web interface does everything routine**, so nothing normal requires
  opening the robot: WiFi and MQTT configuration, the vacuum controls, a live
  log, a UART terminal and firmware updates. It is not a device console — the
  `ota_update` and `wifi_set` commands exist on the USB serial console only.
- **`wifi_status` answers "where is it?"** when the USB port *is* reachable.
  The address is otherwise only in a boot log line that has long since scrolled
  past:

  ```
  neato> wifi_status
  I (48213) console: State: connected
  I (48213) console: Associated with 'Penguin', RSSI: -70 dBm
  I (48214) console: Address: http://192.168.123.105
  ```

  It distinguishes an association from a lease rather than calling both
  "connected", so `State: associated` with `Address: none (no DHCP lease)`
  points at the router rather than the radio. When the fallback access point is
  up it reports that too.
- **The LED is the only diagnostic that survives a network failure**, so it
  carries as much state as can be counted by eye. Every 10 seconds it flashes a
  **state group**, and when there is an association to measure, a longer pause
  followed by a **signal group**:

  | State group | Meaning |
  |---|---|
  | 1 flash | Connected, holding an IP address |
  | 2 flashes | Credentials stored but not associated — cannot reach the AP |
  | 3 flashes | Associated but no IP — DHCP is the problem, not the radio |
  | 4 flashes | Fallback access point up, join it to re-provision |
  | 5 flashes | No credentials stored |
  | Continuous fast blink | Factory reset pin held, counting down |
  | Nothing at all | Not running |

  4 flashes takes precedence over 1: a live access point is reported even when
  the station is also connected. That combination happens when the access point
  was raised deliberately with `wifi_ap`, and it is worth seeing — otherwise the
  one diagnostic a sealed robot has would stay silent about a joinable network.

  | Signal group | RSSI |
  |---|---|
  | 1 flash | Weak, below −75dBm |
  | 2 flashes | Fair, −75 to −65dBm |
  | 3 flashes | Good, above −65dBm |

  So `1 ......... 3` is connected with a good signal, and a bare `2` means it
  never associated. The distinction between 2 and 3 flashes is the one that
  cannot be diagnosed any other way once the robot is closed: the radio failing
  and DHCP failing look identical from outside.

  The firmware version and active OTA slot are also reported by `/api/status`
  and shown under Status, so an update can be confirmed without watching the
  boot log — when there is a network to ask over.

### Joining the fallback access point

```
SSID:       neato-<last 3 bytes of the MAC>     e.g. neato-b63919
Passphrase: neato-setup   (bootstrap only — first-run setup replaces it)
Address:    http://neato.setup  (or any address at all — see below)
```

The SSID is derived from the MAC and is shown under Settings while the device is
still reachable — **note it and the passphrase you set down before sealing the
robot**.

**Joining opens the setup page by itself.** While the access point is up the
device runs a small DNS responder that answers every name with its own address,
and hands itself out as the DNS server in the DHCP lease. A phone or laptop
probes a known URL to decide whether a network reaches the internet; that probe
lands here, gets redirected to `/`, and the client shows the page in its
sign-in window.

Without it the probe fails, the client decides the network is broken and
wanders back to one with internet — while you are standing next to the robot
trying to re-provision it. That is not hypothetical; it is what the AP did
before this existed.

**Then it gets out of the way, for good.** The window is held open only while
the device still needs a password. That is the one moment it earns its place: a
freshly flashed device has to be found and set up, and the window opening by
itself is what makes that happen without anyone being told an address.

The moment first-run setup completes, those same probes start being answered the
way each platform expects instead of being redirected. The client marks the
network usable, the window closes, and the device is reachable at
`http://neato.setup` in an ordinary browser — which is where you want to be for
anything beyond setting a password. Setup deliberately ends on a "you can close
this window" screen rather than dropping you into the interface, because a panel
that is about to close is a poor place to be typing WiFi credentials.

A device that already has a password never captures a client at all: rejoin the
setup network later to re-provision and nothing pops up, so go straight to
`http://neato.setup`.

Note that you have no internet while joined to this network, and every hostname
resolves to the device, so any URL you try will show its page. That is expected:
it is a recovery network, not a route to anywhere. It also means the name is a
convenience rather than a requirement — anything you care to type lands on the
same page.

The numeric address is deliberately not written down anywhere outside
`AP_IP` in `main/wifi.cpp`. The device reports it: the boot log prints it beside
the name, and the sign-in page shows it once the access point is up.

### If the browser says the connection was refused

This is why the name above is the documented way in, and why no numeric address
appears in this file or in the interface.

A browser remembers what it has met at an address, and a private address is a
home router more often than it is anything else. If yours has ever opened a
router's admin page at the one this device uses, it may have cached that
router's redirect to its own HTTPS interface — one case seen here carried
`Cache-Control: max-age=31536000`, making a temporary redirect stick for a year.

The browser then sends every request for that address to port 443 without
consulting the network at all. Nothing listens there, so it reports
`ERR_CONNECTION_REFUSED`, and **nothing appears in the device's log, because the
request never arrived**. `curl` and `wget` keep no such cache and fetch the page
from port 80 quite happily, which makes it look like a browser-specific fault in
the device. It is not; the device never sees it.

Three ways out, cheapest first:

- **Use a name instead.** The captive DNS answers every name with this address,
  so `http://neato.setup` — or anything else you type — reaches the same page
  from a browser that has no history with it. This always works and needs
  nothing cleared.
- **Confirm it in a private window.** Private browsing shares HSTS state but not
  the HTTP cache. If the page loads there and not in a normal window, a cached
  redirect is the cause.
- **Clear it.** `Clear browsing data → Cached images and files` drops the stale
  entry for good.

This cannot be fixed from the device. Answering the HTTPS request in order to redirect
back to `http` would mean terminating TLS, which means a certificate — and no CA
issues trusted certificates for a private address, so every visit would open with
a full-page security warning instead. That is worse than the problem.

The network is always WPA2. There is deliberately no way to make it open: the
interface it serves can drive the vacuum, rewrite the WiFi credentials and
install firmware, and a request arriving over the network must not be able to
remove the authentication from the one interface that exists to recover the
device.

`neato-setup` is published in this repository and the SSID is derived from the
MAC, so a device still using it is findable and joinable by anyone in range.
That is why **first-run setup will not complete until you replace it** — see
below. It can be changed again at any time under **Settings → Setup Access
Point**; the stored value takes precedence and applies the next time the access
point starts.

A stored passphrase survives restarts and OTA updates, but not a factory reset,
which returns the device to the bootstrap default and to first-run setup. So if
you set your own and forget it, GPIO10 is the way back in — that is what the pin
is for. The Status tab also warns whenever the default is in use, because the
NVS erase that `nvs_flash_init()` performs on its own when it runs out of free
pages puts it back silently.

## Security

The web interface is reachable from the whole home network when the station is
up, and from anyone in radio range when the fallback access point is up. It is
gated accordingly.

**First run.** A freshly flashed or factory-reset device serves one thing: a
setup screen. It asks for an interface password and a new access point
passphrase, and refuses to finish until both are set. Until then every other
endpoint answers `403 setup required`. Do this over the fallback access point,
before the robot is sealed.

**Sessions.** The password is stored as a random salt plus a PBKDF2-HMAC-SHA256
digest, so reading the flash out over USB does not hand over the password
itself. Signing in returns a random 128-bit token in an `HttpOnly`,
`SameSite=Strict` cookie; tokens live in RAM only, so a restart signs everyone
out. Repeated failures are throttled rather than locked out permanently — a
permanent lockout on a sealed device is a self-inflicted brick.

**Every endpoint is behind a session**, including the WebSocket upgrade. That
one matters most: a WebSocket is not covered by the same-origin policy, so
without a check on the upgrade any page in any tab could open one to the device
and both drive it and read its log. State-changing requests must additionally
carry an `Origin` matching `Host` and a `Content-Type: application/json`, which
between them stop a cross-origin form or a `no-cors` fetch from reaching a
handler.

The log stream is treated as privileged for the same reason: it carries the
station's SSID, the broker URI and whatever a future log line happens to
include.

**Scripting it.** Sign in and reuse the token as a bearer:

```bash
TOKEN=$(curl -s -X POST http://neato.local/api/login \
  -H 'Content-Type: application/json' -H 'Origin: http://neato.local' \
  -d '{"password":"..."}' | jq -r .token)

curl -s http://neato.local/api/status -H "Authorization: Bearer $TOKEN"
```

A bearer token skips the `Origin` check, since no cross-origin page can set that
header. Cookie-authenticated requests cannot skip it. The unauthenticated
`/api/login` and `/api/setup` always require `Origin` — pass it explicitly as
above.

### Accepted risks

- **No secure boot and no signed images.** The device installs whatever the OTA
  URL serves, so `/api/ota` is only as strong as the session behind it. Enabling
  `CONFIG_SECURE_BOOT` would fix this and is a one-way eFuse operation; on a
  home project it costs more in day-to-day friction than it buys.
- **No flash encryption and no NVS encryption.** The WiFi PSK, the access point
  passphrase and the broker URI sit in plaintext in flash. With ROM download
  mode enabled, brief physical access to the USB port recovers them with
  `esptool read_flash`. Also a one-way eFuse, also deliberately skipped.
- **Plain HTTP.** The interface password and the session cookie cross the LAN in
  the clear. TLS on the device would need a certificate the browser trusts for a
  changing local address, which is not worth the trouble here — but it does mean
  anyone already sniffing your WiFi can take a session.
- **MQTT credentials in the URI leak to anyone with a session.** They are
  redacted in the log and in `/api/status` (`mqtt://***@host`), but the full URI
  is still stored in plaintext NVS.

## Power

The controller runs off the robot's battery, so it is tuned for a device that
idles most of the time rather than for throughput:

- **CPU at 80MHz** instead of 160, roughly halving dynamic power. The visible
  cost is slower OTA downloads and marginally slower page loads.
- **WiFi power save left at the default** (`WIFI_PS_MIN_MODEM`), which already
  sleeps between DTIM beacons. `MAX_MODEM` with a longer listen interval was
  tried and reverted: sleeping through beacons costs latency and reliability on
  a signal the robot's shell already attenuates, and the retransmissions give
  the saving back.
- **The status LED beats once every 10 seconds**, down from being lit roughly
  half the time.
- WiFi sleep code is kept in IRAM, which shortens each wake-up.

Two things deliberately left alone:

- **Automatic light sleep** (`CONFIG_PM_ENABLE` with tickless idle) is *not*
  enabled. The UART driver stays installed to serve the robot link, and bytes
  arriving while the CPU is in light sleep are lost unless UART wake-up is
  configured. Losing the robot's replies to save power is the wrong trade for
  this device. It can be revisited by making the serial terminal release the
  UART when idle.
- **TX power** is left at the default. Range matters once the board is inside
  the robot's shell, and signal that is too weak costs more in retransmissions
  than it saves.

**The red power LED cannot be turned off in software.** On this board it is
wired directly across the 3V3 rail with no GPIO involved, so removing it means
desoldering it or cutting its trace. At a few milliamps it draws more than the
heartbeat LED now does, so it is worth doing if every milliamp counts.

Radio power save is the tempting knob here and the one to be careful with: on a
weak signal it costs more in retransmissions than it saves.

## Reference

[`docs/neato-serial-protocol.md`](docs/neato-serial-protocol.md) is the Neato
Botvac serial command reference, vendored verbatim from
[OpenNeato](https://github.com/renjfk/OpenNeato) (MIT, see
[`docs/OpenNeato-LICENSE`](docs/OpenNeato-LICENSE)). It covers the command set,
response formats and state machines. Corrections belong upstream rather than in
this copy.

## Development Setup

### Prerequisites
- Docker + Docker Compose (for building)
- [`uv`](https://docs.astral.sh/uv/) on the host Mac — the Makefile runs `esptool` and
  `pyserial` through it, so flashing does not depend on a system Python install

### Common Commands

```bash
make build       # compile the project (in Docker)
make flash       # build + flash (flashes from host)
make monitor     # open serial monitor (from host); Ctrl-C quits
make clear-nvs   # erase stored configuration, keeping the firmware
make erase       # wipe the entire flash
make shell       # interactive container shell
```

`clear-nvs` erases the `nvs` partition only — WiFi credentials, the interface
password, the access point passphrase and the broker URI — and leaves both OTA
slots and `otadata` alone, so the running image and its rollback partner
survive. The device comes back up in first-run setup on its fallback access
point.

It is the same effect as holding the GPIO10 reset pin, which is the point: it
exercises that recovery path without opening the robot, and returns a device to
a clean first boot in a couple of seconds rather than the minutes a full erase
and reflash costs. The offset and size are read out of `partitions.csv`, so the
target follows the layout rather than duplicating it.

Both targets need the serial port to themselves — close `make monitor` first, or
esptool will fail with "the port is busy".

Building runs inside Docker using the ESP-IDF toolchain. Flashing and monitoring run directly on the host where the USB device is accessible.

The serial port is auto-detected from the attached `/dev/cu.usbmodem*` or
`/dev/cu.usbserial*` device. Override it if it picks the wrong one:
```bash
DEVICE=/dev/cu.usbserial-1234 make flash
```

### First-Time Setup

```bash
# Build the Docker image
docker compose build

# Configure the project (one-time)
make shell
# inside container:
idf.py set-target esp32c3
idf.py menuconfig
```

## OTA Updates

Pushing a `v*` tag builds the firmware in CI and publishes it as a GitHub
Release, so the device can pull it over HTTPS:

```bash
git tag -a v0.1.2 -m "Some change"
git push origin v0.1.2
```

Then either paste the URL into **Settings → Firmware Update** in the web
interface, or run it from the USB serial console:

```
neato> ota_update https://github.com/tszabo-ro/esp32-robo-vacuum/releases/latest/download/neato-mqtt-controller.bin
```

The `latest/download/` URL always resolves to the newest release, so it does
not need updating per version. Progress is logged, and the web interface
mirrors the log to the browser under the Log tab.

Images are not signed, so the device installs whatever the URL serves. Point it
only at a build you made — see the accepted risks above.

The image is written to the inactive slot and the device reboots into it. It is
only kept if WiFi comes up and the web server starts; otherwise the bootloader
reverts to the previous image on the next restart, so a broken update cannot
strand the device somewhere it can no longer be reached.

Certificates are verified against the bundled root CAs, so the firmware host
needs a publicly trusted certificate — a self-signed local server is rejected.

## Project Status

Working: build environment, CI and tagged releases, WiFi with a fallback access
point, OTA over HTTPS, MQTT with Home Assistant discovery, the serial bridge, and
a password-protected web interface.

Next: replace the simulated vacuum with the real Neato serial protocol.

## License
TBD
