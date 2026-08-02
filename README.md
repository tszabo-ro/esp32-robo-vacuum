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

## Project Status
🚧 **In Development** - Setting up build environment

## License
TBD
