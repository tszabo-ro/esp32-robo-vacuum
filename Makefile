# Serial port of the board. Auto-detected from what is actually plugged in;
# override explicitly with: make flash DEVICE=/dev/cu.usbmodem1234
DEVICE ?= $(firstword $(wildcard /dev/cu.usbmodem* /dev/cu.usbserial*))

# esptool and pyserial run through uv, so they need no system Python install
# and cannot break when a Python version is upgraded or removed. Override to
# use your own copies, e.g. ESPTOOL=esptool.
ESPTOOL  ?= uvx esptool
MINITERM ?= uvx --from pyserial pyserial-miniterm

# Read out of partitions.csv rather than written here, so this cannot quietly
# erase the wrong region if the layout ever moves.
NVS_OFFSET = $(shell awk -F, '/^[[:space:]]*nvs[[:space:]]*,/ {gsub(/[[:space:]]/,"",$$4); print $$4; exit}' partitions.csv)
NVS_SIZE   = $(shell awk -F, '/^[[:space:]]*nvs[[:space:]]*,/ {gsub(/[[:space:]]/,"",$$5); print $$5; exit}' partitions.csv)

.PHONY: build shell flash monitor erase clear-nvs require-device

# Build the project in Docker (no serial port needed)
build:
	docker compose run --rm esp-idf idf.py build

# Open an interactive shell in the container
shell:
	docker compose run --rm esp-idf

# Fail with a clear message when no board is attached, instead of letting
# esptool fail confusingly on an empty port argument.
require-device:
	@test -n "$(DEVICE)" || { \
		echo "No board found on /dev/cu.usbmodem* or /dev/cu.usbserial*."; \
		echo "Plug it in, or set the port explicitly: make <target> DEVICE=/dev/cu.yourport"; \
		exit 1; \
	}

# Build, then flash from the host (the USB device is not visible to Docker).
# ota_data_initial.bin resets the boot slot to ota_0, so a serial flash always
# lands on the slot the app is written to, even after an OTA moved it to ota_1.
flash: build require-device
	$(ESPTOOL) --chip esp32c3 -p $(DEVICE) -b 460800 \
		--before=default-reset --after=hard-reset write-flash \
		--flash-mode dio --flash-freq 80m --flash-size 4MB \
		0x0 build/bootloader/bootloader.bin \
		0x8000 build/partition_table/partition-table.bin \
		0xd000 build/ota_data_initial.bin \
		0x10000 build/neato-mqtt-controller.bin

# Wipe the whole flash. Needed after changing the partition layout, since a
# stale otadata or NVS region would be interpreted against the new table.
# This erases stored WiFi credentials and the MQTT broker URI.
erase: require-device
	$(ESPTOOL) --chip esp32c3 -p $(DEVICE) erase-flash

# Erase stored configuration only, leaving the firmware in place: WiFi
# credentials, the web interface password, the access point passphrase and the
# broker URI all go, and the device comes back up in first-run setup on its
# fallback access point.
#
# The same thing the GPIO10 factory reset does, which is the point - it is how
# that path gets exercised without opening the robot, and how a device gets back
# to a clean first boot without the several minutes a full erase and reflash
# costs. Unlike `erase` it leaves both OTA slots and otadata untouched, so the
# running image and its rollback partner survive.
clear-nvs: require-device
	@test -n "$(NVS_OFFSET)" -a -n "$(NVS_SIZE)" || { \
		echo "No nvs partition found in partitions.csv"; \
		exit 1; \
	}
	@echo "Erasing nvs at $(NVS_OFFSET), $(NVS_SIZE) bytes (configuration only)"
	$(ESPTOOL) --chip esp32c3 -p $(DEVICE) \
		--before=default-reset --after=hard-reset \
		erase-region $(NVS_OFFSET) $(NVS_SIZE)

# Monitor serial output from the host.
#
# Ctrl-C quits, rather than miniterm's default Ctrl-] which nobody reaches for
# and which leaves the port locked against `make flash` and `make clear-nvs`
# when the window is simply walked away from.
#
# The cost is that Ctrl-C can no longer be typed *through* to the device. That
# is a fair trade here: this port carries the ESP's own console, which has
# nothing long-running to interrupt, and the robot's UART is reached from the
# web interface's Serial tab rather than from this terminal.
monitor: require-device
	$(MINITERM) --exit-char 3 $(DEVICE) 115200
