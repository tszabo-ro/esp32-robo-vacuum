DEVICE ?= /dev/cu.usbmodem1301

.PHONY: build shell flash monitor

# Build the project in Docker (no serial port needed)
build:
	docker compose run --rm esp-idf idf.py build

# Open an interactive shell in the container
shell:
	docker compose run --rm esp-idf

# Flash and monitor from the host using esptool
flash: build
	python3 -m esptool --chip esp32c3 -p $(DEVICE) -b 460800 \
		--before=default-reset --after=hard-reset write-flash \
		--flash-mode dio --flash-freq 80m --flash-size 2MB \
		0x0 build/bootloader/bootloader.bin \
		0x10000 build/neato-mqtt-controller.bin \
		0x8000 build/partition_table/partition-table.bin

# Monitor serial output from the host
monitor:
	python3 -m serial.tools.miniterm $(DEVICE) 115200
