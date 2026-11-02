# VoiceInk Button firmware build/flash. Requires arduino-cli with the m5stack:esp32 core,
# M5Unified and M5GFX installed (see README).

HANDHELD_FQBN := m5stack:esp32:m5stack_stopwatch:USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB
# CDCOnBoot off: the sketch starts USB itself (custom name, remote wakeup, no missed mount event)
DONGLE_FQBN   := m5stack:esp32:m5stack_sticks3:USBMode=default,CDCOnBoot=default,UploadMode=cdc

# Your boards, by MAC (= USB serial number), in boards.mk (not committed; `make boards` lists what's
# connected). Port names follow the USB socket, not the device, so flashing and monitoring look the
# port up by MAC and refuse anything else (tools/find_port.py).
-include boards.mk
NEED_MACS = @test -n "$(HANDHELD_MAC)" -a -n "$(DONGLE_MAC)" || { echo "Set HANDHELD_MAC and DONGLE_MAC in boards.mk (see boards.example.mk; make boards lists connected boards)"; exit 1; }
PORT_OF = $$(python3 tools/find_port.py $(1)) || { echo "$(2) $(1) not connected; not touching any other board"; exit 1; }

COMMON := --build-property "compiler.cpp.extra_flags=-I$(CURDIR)/firmware/common"
# Lets the dongle rename the core's "TinyUSB UAC1" audio interface, and leave the mic out of its
# USB configuration while the handheld is away (see dongle.ino)
DONGLE_LD := --build-property "compiler.c.elf.extra_flags=-Wl,--wrap=tinyusb_add_string_descriptor -Wl,--wrap=tud_descriptor_configuration_cb"

.PHONY: handheld dongle flash-handheld flash-dongle first-flash-dongle monitor-handheld monitor-dongle boards

handheld:
	arduino-cli compile -b $(HANDHELD_FQBN) $(COMMON) --build-path build/handheld firmware/handheld

dongle:
	arduino-cli compile -b $(DONGLE_FQBN) $(COMMON) $(DONGLE_LD) --build-path build/dongle firmware/dongle

flash-handheld: handheld
	$(NEED_MACS)
	@port=$(call PORT_OF,$(HANDHELD_MAC),StopWatch); echo "StopWatch $(HANDHELD_MAC) on $$port"; \
	arduino-cli upload -b $(HANDHELD_FQBN) -p $$port --input-dir build/handheld firmware/handheld

flash-dongle: dongle
	$(NEED_MACS)
	@port=$(call PORT_OF,$(DONGLE_MAC),Dongle); echo "Dongle $(DONGLE_MAC) on $$port"; \
	arduino-cli upload -b $(DONGLE_FQBN) -p $$port --input-dir build/dongle firmware/dongle

# A StickS3 that doesn't run this firmware yet (or crashed): put it in download mode first (hold the
# side button ~2 s until the green LED blinks). Writes the merged image, then boots it.
first-flash-dongle: dongle
	$(NEED_MACS)
	@port=$(call PORT_OF,$(DONGLE_MAC),Dongle); echo "Dongle $(DONGLE_MAC) on $$port"; \
	esptool --port $$port --before no-reset --after no-reset write-flash 0x0 build/dongle/dongle.ino.merged.bin && \
	esptool --port $$port --before no-reset --after watchdog-reset chip-id

monitor-handheld:
	$(NEED_MACS)
	@port=$(call PORT_OF,$(HANDHELD_MAC),StopWatch); arduino-cli monitor -p $$port -c baudrate=115200

monitor-dongle:
	$(NEED_MACS)
	@port=$(call PORT_OF,$(DONGLE_MAC),Dongle); arduino-cli monitor -p $$port -c baudrate=115200

boards:
	@python3 tools/find_port.py --list
