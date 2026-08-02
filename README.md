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
  no credentials are stored at all. Join it and browse to `http://192.168.4.1`
  to reach the same web interface and re-provision. It shuts down again once the
  station reconnects, so it is only exposed while it is needed.
- **The console is reachable from the browser.** The Console tab runs the same
  commands as the serial console, including `ota_update`, so nothing routine
  requires opening the robot.
- **The LED** is the only physical indicator: blinking means the station is
  connected, dark means it is not.

The access point's SSID is derived from the MAC (for example `neato-b63919`) and
is shown under Settings while the device is reachable — **note it down before
sealing the robot**. Its password defaults to `neato-setup`, which is public in
this repository: set your own under Settings, which stores it in NVS.

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
