# ESP32-C3 Neato D5 MQTT Controller

ESP32-C3 SuperMini based controller for Neato D5 robot vacuum, enabling Home Assistant integration via MQTT.

## Features (Planned)
- MQTT communication with Home Assistant
- FreeRTOS-based task management
- Vacuum control and monitoring
- OTA updates
- WiFi configuration

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
  no credentials are stored at all. Join it and browse to `http://192.168.1.1`
  to reach the same web interface and re-provision. It shuts down again once the
  station reconnects, so it is only exposed while it is needed.
- **A factory reset pin** on **GPIO10**: hold it to ground for 5 seconds to
  erase all stored configuration and restart, which brings the access point
  back up. The pin has an internal pull-up, so leaving it unconnected is safe,
  and the LED blinks rapidly while the hold is counting down — release before
  5 seconds to cancel. This is the way back if even the access point cannot be
  reached, for instance because its password was set and then forgotten.
- **The console is reachable from the browser.** The Console tab runs the same
  commands as the serial console, including `ota_update`, so nothing routine
  requires opening the robot.
- **The LED** is the only physical indicator, and beats once every 10 seconds:

  | Pattern | Meaning |
  |---|---|
  | One short flash every 10s | Alive, station connected |
  | Two short flashes every 10s | Alive, station **not** connected |
  | Continuous fast blink | Factory reset pin held, counting down |
  | Nothing at all | Not running |

  The firmware version and active OTA slot are also reported by `/api/status`
  and shown under Status, so an update can be confirmed without watching the
  boot log.

### Joining the fallback access point

```
SSID:     neato-<last 3 bytes of the MAC>     e.g. neato-b63919
Password: neato-setup
Address:  http://192.168.1.1
```

The SSID is derived from the MAC and is shown under Settings while the device is
still reachable — **note it and any password you set down before sealing the
robot**.

The network is WPA2 rather than open deliberately. The interface it serves can
drive the vacuum, rewrite the WiFi credentials and run `ota_update`, so an open
fallback would hand all of that to anyone in range. The default above is public
in this repository, though, so it only deters a passer-by. Change it in one of
three ways:

- **Settings → Setup Access Point** in the web interface. Stored in NVS, takes
  precedence over the built-in default, and applies the next time the access
  point starts. This is the one worth doing before sealing the robot.
- **Submit a blank password** there to make the network open. Anything shorter
  than 8 characters is unusable for WPA2, so it falls back to an open network
  and says so in the log.
- **Change `DEFAULT_AP_PASSWORD`** in `main/wifi.cpp` to ship a different
  built-in default.

A password stored in NVS survives restarts and OTA updates, but not a factory
reset, which returns it to the built-in default. So if you set your own and
forget it, GPIO10 is the way back in — that is what the pin is for.

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
make monitor     # open serial monitor (from host)
make shell       # interactive container shell
```

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

Then, from the serial console (or the web interface's console):

```
neato> ota_update https://github.com/tszabo-ro/esp32-robo-vacuum/releases/latest/download/neato-mqtt-controller.bin
```

The `latest/download/` URL always resolves to the newest release, so it does
not need updating per version. Progress is logged, and the web interface
mirrors the log to the browser.

The image is written to the inactive slot and the device reboots into it. It is
only kept if WiFi comes up and the web server starts; otherwise the bootloader
reverts to the previous image on the next restart, so a broken update cannot
strand the device somewhere it can no longer be reached.

Certificates are verified against the bundled root CAs, so the firmware host
needs a publicly trusted certificate — a self-signed local server is rejected.

## Project Status
🚧 **In Development** - Setting up build environment

## License
TBD
