# Hand-rolled iPod Video firmware.
#   make          build/knurl.ipod      (install as /.knurl/knurl.ipod; Knurl's loader starts it)
#                 build/knurlboot.ipod  (that loader; Knurl installs it into the firmware partition,
#                                        see device/fwinstall.c)
#   make sim      build/knurl-sim   (the same app code, run on Linux)
#   make shots    screenshots of a scripted walk through the UI -> build/shots/*.png
CROSS   ?= arm-none-eabi-
GEN      = build/gen
INC      = -Isrc -I$(GEN)
WARN     = -std=c11 -Wall -Wextra
DEV_CFLAGS = -mcpu=arm7tdmi -marm -O2 $(WARN) -ffreestanding -fno-builtin -nostdlib \
             -ffunction-sections -fdata-sections $(INC) -Idevice
DEV_LDFLAGS = -T link.ld -nostartfiles -nostdlib -Wl,--gc-sections -Wl,-Map=build/knurl.map
SIM_CFLAGS = -O1 -g $(WARN) $(INC) -Isim

HDRS  = $(wildcard src/*.h src/*/*.h src/*/*.inc device/*.h) $(GEN)/fonts.h $(GEN)/icons.h $(GEN)/light.h $(GEN)/logo.h
FONTS = $(wildcard ../assets/fonts/*.ttf)

COMMON = app splash engine player mp3dec mp3info pow43 gfx plane fonts icons light logo fat library mem
DEV_OBJS = $(addprefix build/dev/,crt0.o device.o ata.o audio.o i2c.o power.o usb.o store.o fwinstall.o cop.o libc.o synth_arm.o $(addsuffix .o,$(COMMON)))
BOOT_OBJS = $(addprefix build/boot/,start.o boot.o ata.o fat.o libc.o)
SIM_OBJS = $(addprefix build/sim/,sim.o $(addsuffix .o,$(COMMON)))

vpath %.c src src/gfx src/storage src/library src/mp3 device boot sim $(GEN)
vpath %.S src src/mp3 device boot

# A raw disk image with a FAT32 volume holding a library (tools/make_test_image.py).
DISK ?= ../testdata/demo.img
SHOT_SCRIPT = shot:01-home sel shot:02-albums-flow w+5 shot:03-albums-flow-turned play shot:04-albums-grid play \
              sel shot:05-album w+2 sel t+3000 shot:06-now-playing w+3 shot:07-volume t+2000 sel shot:08-scrubbing \
              w+4 shot:09-scrub-target t+400 shot:10-after-seek menu menu shot:11-album-mini menu \
              shot:12-albums-flow-mini menu w+1 sel shot:13-artists-flow play shot:14-artists-list play \
              w+2 sel shot:15-artist-flow menu menu w+1 sel shot:16-songs-flow w+20 shot:17-songs-flow-turned \
              menu w-4 sel shot:18-recent-flow menu w+7 sel shot:19-settings w+2 sel w+3 shot:20-settings-editing \
              sel w+3 sel shot:21-about menu menu w+1 shot:22-home-now-row

all: build/knurl.ipod build/knurlboot.ipod

build/dev build/boot build/sim $(GEN) build/shots:
	mkdir -p $@

$(GEN)/fonts.c $(GEN)/fonts.h &: ../tools/build_fonts.py $(FONTS) | $(GEN)
	python3 ../tools/build_fonts.py $(GEN)

$(GEN)/icons.c $(GEN)/icons.h &: ../tools/build_icons.py | $(GEN)
	python3 ../tools/build_icons.py $(GEN)

$(GEN)/light.c $(GEN)/light.h &: ../tools/build_light.py | $(GEN)
	python3 ../tools/build_light.py $(GEN)

$(GEN)/logo.c $(GEN)/logo.h &: ../tools/build_logo.py | $(GEN)
	python3 ../tools/build_logo.py $(GEN)

$(GEN)/pow43.c: ../tools/build_pow43.py | $(GEN)
	python3 ../tools/build_pow43.py $(GEN)

# ---- device ----
build/dev/%.o: %.c $(HDRS) | build/dev
	$(CROSS)gcc $(DEV_CFLAGS) -c $< -o $@
build/dev/%.o: %.S | build/dev
	$(CROSS)gcc $(DEV_CFLAGS) -c $< -o $@

build/knurl.elf: $(DEV_OBJS) link.ld
	$(CROSS)gcc $(DEV_CFLAGS) $(DEV_LDFLAGS) -o $@ $(DEV_OBJS) -lgcc
build/knurl.bin: build/knurl.elf
	$(CROSS)objcopy -O binary $< $@
build/knurl.ipod: build/knurl.bin ../tools/mkipod.py
	python3 ../tools/mkipod.py $< $@

# ---- boot loader (runs from IRAM, see boot/start.S) ----
build/boot/%.o: %.c $(HDRS) | build/boot
	$(CROSS)gcc $(DEV_CFLAGS) -c $< -o $@
build/boot/%.o: %.S | build/boot
	$(CROSS)gcc $(DEV_CFLAGS) -c $< -o $@

build/knurlboot.elf: $(BOOT_OBJS) boot/link.ld
	$(CROSS)gcc $(DEV_CFLAGS) -T boot/link.ld -nostartfiles -nostdlib -Wl,--gc-sections \
	    -Wl,-Map=build/knurlboot.map -o $@ $(BOOT_OBJS) -lgcc
build/knurlboot.bin: build/knurlboot.elf
	$(CROSS)objcopy -O binary $< $@
build/knurlboot.ipod: build/knurlboot.bin ../tools/mkipod.py
	python3 ../tools/mkipod.py $< $@

# ---- simulator ----
build/sim/%.o: %.c $(HDRS) | build/sim
	gcc $(SIM_CFLAGS) -c $< -o $@
build/sim/%.o: %.S | build/sim
	gcc -c $< -o $@
build/knurl-sim: $(SIM_OBJS)
	gcc -z noexecstack -o $@ $(SIM_OBJS)
sim: build/knurl-sim

shots: build/knurl-sim | build/shots
	rm -f build/shots/*
	build/knurl-sim --disk $(DISK) --boot-shot 00-boot build/shots $(SHOT_SCRIPT)
	build/knurl-sim build/shots shot:00-no-disk
	python3 ../tools/shots.py build/shots

clean:
	rm -rf build

.PHONY: all sim shots clean
