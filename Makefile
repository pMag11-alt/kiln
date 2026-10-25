# kiln: build the kernel, the user programs and an ext2 root image, and run
# them under QEMU.
#
#   make            kernel + root file system image
#   make qemu       boot it (Ctrl-A X quits QEMU)
#   make test       boot, run the test suites, then fsck the image
#   make fresh      rebuild the disk image from scratch (discards changes)

BUILD   := build
CPUS    ?= 4
MEM     ?= 128M
BOOTARGS ?=

# ---- toolchain: clang + lld, plus e2fsprogs for the disk image ----
UNAME := $(shell uname -s)
ifeq ($(UNAME),Darwin)
  BREW  := $(shell brew --prefix 2>/dev/null)
  LLVM  ?= $(BREW)/opt/llvm/bin/
  LLD   ?= $(firstword $(wildcard $(BREW)/opt/lld/bin/ld.lld $(BREW)/opt/lld@*/bin/ld.lld $(LLVM)ld.lld))
  E2FS  ?= $(BREW)/opt/e2fsprogs/sbin/
else
  LLVM  ?=
  LLD   ?= ld.lld
  E2FS  ?=
endif
CC        := $(LLVM)clang
OBJCOPY   := $(LLVM)llvm-objcopy
OBJDUMP   := $(LLVM)llvm-objdump
ADDR2LINE := $(LLVM)llvm-addr2line
LD        := $(LLD)
MKE2FS    := $(E2FS)mke2fs
E2FSCK    := $(E2FS)e2fsck
QEMU      ?= qemu-system-riscv64
PYTHON    ?= python3

ARCH := --target=riscv64-unknown-elf -march=rv64imac_zicsr_zifencei -mabi=lp64 \
        -mcmodel=medany -mno-relax
CFLAGS := $(ARCH) -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter \
          -ffreestanding -fno-builtin -nostdlib -fno-common -fno-pic \
          -fno-stack-protector -fno-omit-frame-pointer -fno-strict-aliasing \
          -ffunction-sections -fdata-sections -Iinclude -MMD -MP
ASFLAGS := $(ARCH) -Iinclude -Ikernel -MMD -MP
LDFLAGS := -nostdlib -z max-page-size=4096 --gc-sections

# ---- kernel ----
KSRCS := $(wildcard kernel/*.c) lib/string.c lib/fmt.c
KASM  := kernel/entry.S kernel/trap.S
KOBJS := $(patsubst %.S,$(BUILD)/%.o,$(KASM)) $(patsubst %.c,$(BUILD)/k/%.o,$(KSRCS))

# ---- user space ----
ULIB_C   := user/lib/ulib.c user/lib/printf.c user/lib/malloc.c user/lib/dirent.c \
            lib/string.c lib/fmt.c
ULIB_S   := user/lib/crt0.S user/lib/usys.S
ULIB_OBJ := $(patsubst %.S,$(BUILD)/%.o,$(ULIB_S)) $(patsubst %.c,$(BUILD)/u/%.o,$(ULIB_C))
PROGS    := $(basename $(notdir $(wildcard user/*.c)))
UBINS    := $(addprefix $(BUILD)/root/bin/,$(PROGS))

FS_IMG := $(BUILD)/fs.img

all: $(BUILD)/kernel.bin $(FS_IMG)

Q := @
ifeq ($(V),1)
  Q :=
endif

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS      $<"
	$(Q)$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/k/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -Ikernel -c $< -o $@

$(BUILD)/kernel.elf: $(KOBJS) kernel/kernel.ld
	@echo "  LD      $@"
	$(Q)$(LD) $(LDFLAGS) -T kernel/kernel.ld -o $@ $(KOBJS)

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(Q)$(OBJCOPY) -O binary $< $@

$(BUILD)/kernel.asm: $(BUILD)/kernel.elf
	$(OBJDUMP) -d -S $< > $@

$(BUILD)/u/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -Iuser/lib -c $< -o $@

$(BUILD)/root/bin/%: $(BUILD)/u/user/%.o $(ULIB_OBJ) user/lib/user.ld
	@mkdir -p $(dir $@)
	$(Q)$(LD) $(LDFLAGS) -T user/lib/user.ld -o $@ $< $(ULIB_OBJ)

# The root file system: everything under rootfs/ plus the programs, packed
# into a plain ext2 image (1 KiB blocks, 128-byte inodes, no ext3/4 bits).
ROOTFS_FILES := $(shell find rootfs -type f 2>/dev/null)
$(FS_IMG): $(UBINS) $(ROOTFS_FILES) $(BUILD)/kernel.elf
	@rm -rf $(BUILD)/root/etc $(BUILD)/root/home $(BUILD)/root/usr
	@mkdir -p $(BUILD)/root/dev $(BUILD)/root/tmp $(BUILD)/root/usr/share
	@cp -R rootfs/. $(BUILD)/root/
	@cp $(BUILD)/kernel.elf $(BUILD)/root/usr/share/kernel.elf
	@rm -f $@
	@echo "  MKE2FS  $@"
	$(Q)$(MKE2FS) -q -F -t ext2 -b 1024 -I 128 -N 4096 -O none,filetype,sparse_super \
	  -E root_owner=0:0 -L kiln -d $(BUILD)/root $@ 64M

fresh:
	rm -f $(FS_IMG)
	$(MAKE) $(FS_IMG)

QEMUOPTS = -machine virt -bios default -smp $(CPUS) -m $(MEM) -nographic \
           -kernel $(BUILD)/kernel.bin -global virtio-mmio.force-legacy=false \
           -drive file=$(FS_IMG),if=none,format=raw,id=hd0 \
           -device virtio-blk-device,drive=hd0,bus=virtio-mmio-bus.0 \
           -append "$(BOOTARGS)"

qemu: all
	$(QEMU) $(QEMUOPTS)

qemu-gdb: all
	$(QEMU) $(QEMUOPTS) -S -gdb tcp::1234

test: all
	$(PYTHON) scripts/test.py --qemu $(QEMU) --e2fsck $(E2FSCK) --debugfs $(E2FS)debugfs \
	  --cpus $(CPUS)

fsck:
	$(E2FSCK) -fn $(FS_IMG)

# make addr2line ADDRS="0xffffffc080201234 ..."  (addresses from a panic)
addr2line: $(BUILD)/kernel.elf
	$(ADDR2LINE) -f -p -e $< $(ADDRS)

clean:
	rm -rf $(BUILD)

.PHONY: all fresh qemu qemu-gdb test fsck addr2line clean
.SECONDARY:
.DELETE_ON_ERROR:

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
