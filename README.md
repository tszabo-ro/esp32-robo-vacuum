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
