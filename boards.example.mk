# Copy to boards.mk and fill in your boards' MACs. `make boards` lists the connected ESP32-S3s.
# A StopWatch shows up as "USB JTAG_serial debug unit"; a dongle running this firmware as
# "VoiceInk Button Dongle". Flashing and monitoring only ever touch these two.
HANDHELD_MAC := AA:BB:CC:DD:EE:01
DONGLE_MAC   := AA:BB:CC:DD:EE:02
