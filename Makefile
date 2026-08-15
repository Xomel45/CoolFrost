C_SOURCES = $(wildcard kernel/*.c drivers/*.c cpu/*.c libc/*.c power/*.c fs/*.c net/*.c gfx/*.c ui/*.c user/*.c)
OBJ       = ${C_SOURCES:.c=.o} cpu/interrupt.o drivers/cpuid-detect.o \
            boot/ap_trampoline_embed.o

CC     = x86_64-elf-gcc
LD     = x86_64-elf-ld
GDB    = gdb

# 64-bit freestanding kernel flags
# -mno-red-zone: disable the 128-byte red zone (mandatory for interrupt handlers)
# -fno-pic:      no position-independent code (we link at a fixed address)
# -MMD -MP:      emit a per-.o .d file listing the headers it actually
#                includes (picked up below via -include), so touching one
#                header only rebuilds the .c files that include it — not
#                the old blanket "every .o depends on every header in the
#                tree" rule, which rebuilt all ~60 objects on any .h touch.
CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions \
         -m64 -mno-red-zone -fno-pic -fno-stack-protector -MMD -MP

# Standalone freestanding ELF built OUTSIDE kernel.elf entirely — runs in
# its own address space (cpu/vmm.h) loaded by kernel/elf.c's `exec` command.
# Companion linker script (userland/user.ld) pins the load address to match.
USER_CFLAGS = -ffreestanding -nostdlib -static -no-pie -m64 -mno-red-zone \
              -fno-pic -fno-stack-protector -Wall -Wextra

userland/hello.elf: userland/hello.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/hello.c

userland/crash.elf: userland/crash.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/crash.c

userland/cat.elf: userland/cat.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/cat.c

userland/badptr.elf: userland/badptr.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/badptr.c

userland/wtest.elf: userland/wtest.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/wtest.c

userland/ctest.elf: userland/ctest.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/ctest.c

userland/etest.elf: userland/etest.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/etest.c

userland/stest.elf: userland/stest.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/stest.c

userland/dtest.elf: userland/dtest.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/dtest.c

userland/mtest.elf: userland/mtest.c userland/usyscall.h userland/umalloc.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/mtest.c

userland/rtest.elf: userland/rtest.c userland/usyscall.h userland/ufileutil.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/rtest.c

userland/gtest.elf: userland/gtest.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/gtest.c

userland/fragtest.elf: userland/fragtest.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/fragtest.c

userland/dftest.elf: userland/dftest.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/dftest.c

userland/edftest.elf: userland/edftest.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/edftest.c

# Test disk: MBR + one FAT32 (LBA) partition at LBA 2048 containing
# userland/hello.elf + crash.elf + cat.elf + badptr.elf + wtest.elf +
# ctest.elf (+ greeting.txt/second.txt, read by cat.elf via SYS_OPEN/
# SYS_READ — second.txt specifically exercises argv[1] (kernel/elf.c) and
# gets overwritten in place by wtest.elf via SYS_FWRITE (fs/fat32.c:
# fat32_write)), for `exec /hda1/<name> [args...]`. ctest.elf creates its
# own brand-new file at runtime (fs/fat32.c: fat32_create) — nothing extra
# to seed here for it. mtools' `@@offset` syntax formats/copies directly
# into the image file — no loop device, no root. Filenames here are kept to
# FAT32 8.3 short-name length on purpose — fs/fat32.c's finddir/readdir
# skip LFN entries entirely (fat32_entry_valid), so anything mtools would
# have to give a generated short name (like WRITE_~1.ELF for a literal
# "write_test.elf") is unreachable by its real name through this VFS.
# Second test partition: ext2, for fs/ext2.c's new write path (create/grow
# files — read support predates this). Built as a standalone 64MiB image
# via mke2fs — works directly on a plain file, no loop device or root
# needed — then dd'd into hdd.img at the second MBR partition's byte
# offset. Nothing needs pre-seeding onto it: userland/etest.c creates its
# own file at runtime, mirroring what userland/ctest.c does for FAT32.
# 1024-byte blocks (not mke2fs's size-based default) so 12 direct blocks
# is only 12KB — small enough that a modest test write forces growth past
# them into the single-indirect range fs/ext2.c: get_or_alloc_block covers.
ext2_part.img:
	dd if=/dev/zero of=$@ bs=1M count=64
	mke2fs -F -q -t ext2 -b 1024 $@

# Third test partition: xfs, for fs/xfs.c (read-only — no write path at
# all, unlike fat32/ext2). mkfs.xfs refuses anything under 300MiB, hence
# the much bigger image than ext2_part.img's. Seeded via mkfs.xfs's own -p
# "populate from directory" flag AT CREATE TIME — the only way to get test
# content onto it, since nothing here can write to xfs afterward. -m
# crc=0 picks the classic V4 on-disk format (fs/xfs.h explains why: V5's
# CRC32C+LSN+UUID header on every metadata structure is real complexity
# this first implementation doesn't take on). manyfiles/ holds 60 entries
# specifically to push its directory past shortform into XFS's
# single-block "extents" format — the other on-disk directory layout
# fs/xfs.c supports, exercised by userland/xtest.c.
xfs_seed/small.txt:
	mkdir -p xfs_seed
	echo -n "hello from a real xfs volume" > $@

xfs_seed/big.txt:
	mkdir -p xfs_seed
	python3 -c "open('$@','wb').write(bytes((i&0xFF) for i in range(70000)))"

xfs_seed/manyfiles/.stamp:
	mkdir -p xfs_seed/manyfiles
	for i in $$(seq 1 60); do echo -n "content $$i" > xfs_seed/manyfiles/file$$(printf '%03d' $$i).txt; done
	touch $@

xfs_part.img: xfs_seed/small.txt xfs_seed/big.txt xfs_seed/manyfiles/.stamp
	dd if=/dev/zero of=$@ bs=1M count=320
	mkfs.xfs -f -q -b size=1024 -m crc=0 -p xfs_seed $@

# Test disk: MBR + partition 0 = FAT32 (LBA 2048, 129024 sectors) +
# partition 1 = ext2 (LBA 131072, 131072 sectors) + partition 2 = xfs (LBA
# 262144, 655360 sectors = 320MiB) appended after that — hence the file
# growing to 448MiB (262144+655360 sectors exactly, no slack) and
# mformat's `-T` pinning FAT32 to its original size instead of claiming
# the whole now-bigger file. Partition 2's MBR type byte is 0x83, same as
# ext2's — real Linux doesn't reserve a separate byte for xfs either, see
# fs/vfs.c: vfs_mount's ext2-then-xfs probe fallback. See fs/fat32.c /
# fs/ext2.c / fs/xfs.c doc comments for what each userland/*.elf test
# program exercises.
hdd.img: userland/hello.elf userland/crash.elf userland/cat.elf userland/badptr.elf userland/wtest.elf userland/ctest.elf userland/etest.elf userland/stest.elf userland/dtest.elf userland/mtest.elf userland/rtest.elf userland/gtest.elf userland/fragtest.elf userland/dftest.elf userland/edftest.elf userland/xtest.elf userland/greeting.txt userland/second.txt ext2_part.img xfs_part.img
	dd if=/dev/zero of=$@ bs=1M count=448
	mformat -i $@@@1M -F -T 129024 ::
	mcopy -i $@@@1M userland/hello.elf ::/hello.elf
	mcopy -i $@@@1M userland/crash.elf ::/crash.elf
	mcopy -i $@@@1M userland/cat.elf ::/cat.elf
	mcopy -i $@@@1M userland/badptr.elf ::/badptr.elf
	mcopy -i $@@@1M userland/wtest.elf ::/wtest.elf
	mcopy -i $@@@1M userland/ctest.elf ::/ctest.elf
	mcopy -i $@@@1M userland/etest.elf ::/etest.elf
	mcopy -i $@@@1M userland/stest.elf ::/stest.elf
	mcopy -i $@@@1M userland/dtest.elf ::/dtest.elf
	mcopy -i $@@@1M userland/mtest.elf ::/mtest.elf
	mcopy -i $@@@1M userland/rtest.elf ::/rtest.elf
	mcopy -i $@@@1M userland/gtest.elf ::/gtest.elf
	mcopy -i $@@@1M userland/fragtest.elf ::/fragtest.elf
	mcopy -i $@@@1M userland/dftest.elf ::/dftest.elf
	mcopy -i $@@@1M userland/edftest.elf ::/edftest.elf
	mcopy -i $@@@1M userland/xtest.elf ::/xtest.elf
	mcopy -i $@@@1M userland/greeting.txt ::/greeting.txt
	mcopy -i $@@@1M userland/second.txt ::/second.txt
	dd if=ext2_part.img of=$@ bs=512 seek=131072 conv=notrunc
	dd if=xfs_part.img of=$@ bs=512 seek=262144 conv=notrunc
	python3 -c "import struct; f=open('$@','r+b'); f.seek(446); f.write(struct.pack('<BBBBBBBBII',0,0xFE,0xFF,0xFF,0x0C,0xFE,0xFF,0xFF,2048,129024)); f.seek(462); f.write(struct.pack('<BBBBBBBBII',0,0xFE,0xFF,0xFF,0x83,0xFE,0xFF,0xFF,131072,131072)); f.seek(478); f.write(struct.pack('<BBBBBBBBII',0,0xFE,0xFF,0xFF,0x83,0xFE,0xFF,0xFF,262144,655360)); f.seek(510); f.write(b'\x55\xAA'); f.close()"

# Flat-binary AP trampoline → embedded into the kernel as a blob
boot/ap_trampoline.bin: boot/ap_trampoline.asm
	nasm -f bin -o $@ $<

boot/ap_trampoline_embed.o: boot/ap_trampoline_embed.asm boot/ap_trampoline.bin
	nasm -f elf64 -o $@ $<

kernel.elf: boot/multiboot_entry.o ${OBJ}
	${LD} -m elf_x86_64 -o $@ -T linker.ld $^

iso: kernel.elf
	mkdir -p ./iso/boot/grub/
	cp ./grub.cfg ./iso/boot/grub/grub.cfg
	cp ./kernel.elf ./iso/boot/kernel.elf
	grub-mkrescue -o CoolFrost.iso iso/

run-grub: kernel.elf
	qemu-system-x86_64 -m 512M -kernel kernel.elf \
	    -drive file=hdd.img,format=raw,if=ide \
	    -audiodev pa,id=speaker \
	    -machine pcspk-audiodev=speaker \
	    -enable-kvm -cpu host -vga std

run-net: kernel.elf
	qemu-system-x86_64 -m 512M -kernel kernel.elf \
	    -drive file=hdd.img,format=raw,if=ide \
	    -nic user,model=e1000 \
	    -audiodev pa,id=speaker \
	    -machine pcspk-audiodev=speaker \
	    -enable-kvm -cpu host -vga std

sata.img:
	dd if=/dev/zero of=sata.img bs=1M count=256

vdisk.img:
	dd if=/dev/zero of=vdisk.img bs=1M count=256

run-virtio: kernel.elf
	qemu-system-x86_64 -m 512M -kernel kernel.elf \
	    -drive file=hdd.img,format=raw,if=ide \
	    -drive file=vdisk.img,format=raw,if=none,id=vdisk0 \
	    -device virtio-blk-pci,drive=vdisk0 \
	    -audiodev pa,id=speaker \
	    -device AC97,audiodev=speaker \
	    -machine pcspk-audiodev=speaker \
	    -serial stdio \
	    -enable-kvm -cpu host -vga std

run-ahci: kernel.elf
	qemu-system-x86_64 -m 512M -kernel kernel.elf \
	    -drive file=hdd.img,format=raw,if=ide \
	    -device ich9-ahci,id=ahci0 \
	    -drive file=sata.img,format=raw,if=none,id=sata0 \
	    -device ide-hd,drive=sata0,bus=ahci0.0 \
	    -serial stdio \
	    -audiodev pa,id=speaker \
	    -machine pcspk-audiodev=speaker \
	    -enable-kvm -cpu host -vga std

run-nvme: kernel.elf
	qemu-system-x86_64 -m 512M -kernel kernel.elf \
	    -drive file=hdd.img,format=raw,if=ide \
	    -drive file=nvme.img,format=raw,if=none,id=nvme0 \
	    -device nvme,serial=deadbeef,drive=nvme0 \
	    -audiodev pa,id=speaker \
	    -machine pcspk-audiodev=speaker \
	    -enable-kvm -cpu host -vga std

run-iso: CoolFrost.iso hdd.img
	qemu-system-x86_64 -m 512M -boot d -cdrom CoolFrost.iso \
	    -drive file=hdd.img,format=raw,if=ide \
	    -audiodev pa,id=speaker \
	    -machine pcspk-audiodev=speaker \
	    -enable-kvm -cpu host -vga std

debug: kernel.elf
	qemu-system-x86_64 -S -s -m 512M -kernel kernel.elf \
	    -drive file=hdd.img,format=raw,if=ide \
	    -audiodev pa,id=speaker \
	    -machine pcspk-audiodev=speaker \
	    -d guest_errors,int &
	${GDB} -ex "target remote localhost:1234" \
	       -ex "symbol-file kernel.elf"

check: ${C_SOURCES}
	${CC} -ffreestanding -fsyntax-only -Wall -Wextra -m64 -mno-red-zone -fno-pic -c $^

%.o: %.c
	${CC} ${CFLAGS} -c $< -o $@

%.o: %.asm
	nasm $< -f elf64 -o $@

clean:
	rm -rf *.iso *.dis *.o *.elf hdd.img ext2_part.img xfs_part.img xfs_seed
	rm -rf kernel/*.o kernel/*.d boot/*.o boot/*.bin drivers/*.o drivers/*.d \
	       cpu/*.o cpu/*.d libc/*.o libc/*.d power/*.o power/*.d vm/*.o vm/*.d \
	       net/*.o net/*.d user/*.o user/*.d fs/*.o fs/*.d gfx/*.o gfx/*.d ui/*.o ui/*.d
	rm -rf userland/*.elf
	rm -rf iso/

# Per-object header dependencies emitted by -MMD above (silently skipped
# for entries with no .d — cpu/interrupt.o etc. are assembled by nasm, not
# gcc, so they never get one).
-include $(OBJ:.o=.d)
