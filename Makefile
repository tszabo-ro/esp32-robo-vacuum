# Serial port of the board. Auto-detected from what is actually plugged in;
# override explicitly with: make flash DEVICE=/dev/cu.usbmodem1234
DEVICE ?= $(firstword $(wildcard /dev/cu.usbmodem* /dev/cu.usbserial*))

# esptool and pyserial run through uv, so they need no system Python install
# and cannot break when a Python version is upgraded or removed. Override to
# use your own copies, e.g. ESPTOOL=esptool.
ESPTOOL  ?= uvx esptool
MINITERM ?= uvx --from pyserial pyserial-miniterm

.PHONY: build shell flash monitor erase require-device

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

# Monitor serial output from the host
monitor: require-device
	$(MINITERM) $(DEVICE) 115200
