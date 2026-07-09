C_SOURCES = $(wildcard kernel/*.c drivers/*.c cpu/*.c libc/*.c power/*.c fs/*.c net/*.c gfx/*.c ui/*.c user/*.c)
HEADERS   = $(wildcard kernel/*.h drivers/*.h cpu/*.h libc/*.h power/*.h fs/*.h net/*.h gfx/*.h ui/*.h user/*.h)
OBJ       = ${C_SOURCES:.c=.o} cpu/interrupt.o drivers/cpuid-detect.o \
            boot/ap_trampoline_embed.o

CC     = x86_64-elf-gcc
LD     = x86_64-elf-ld
GDB    = gdb

# 64-bit freestanding kernel flags
# -mno-red-zone: disable the 128-byte red zone (mandatory for interrupt handlers)
# -fno-pic:      no position-independent code (we link at a fixed address)
CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions \
         -m64 -mno-red-zone -fno-pic -fno-stack-protector

# Standalone freestanding ELF built OUTSIDE kernel.elf entirely — runs in
# its own address space (cpu/vmm.h) loaded by kernel/elf.c's `exec` command.
# Companion linker script (userland/user.ld) pins the load address to match.
USER_CFLAGS = -ffreestanding -nostdlib -static -no-pie -m64 -mno-red-zone \
              -fno-pic -fno-stack-protector -Wall -Wextra

userland/hello.elf: userland/hello.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/hello.c

userland/crash.elf: userland/crash.c userland/usyscall.h userland/user.ld
	${CC} ${USER_CFLAGS} -T userland/user.ld -o $@ userland/crash.c

# Test disk: MBR + one FAT32 (LBA) partition at LBA 2048 containing
# userland/hello.elf + crash.elf, for `exec /hda1/hello.elf`. mtools'
# `@@offset` syntax formats/copies directly into the image file — no loop
# device, no root.
hdd.img: userland/hello.elf userland/crash.elf
	dd if=/dev/zero of=$@ bs=1M count=64
	mformat -i $@@@1M -F ::
	mcopy -i $@@@1M userland/hello.elf ::/hello.elf
	mcopy -i $@@@1M userland/crash.elf ::/crash.elf
	python3 -c "import struct; f=open('$@','r+b'); f.seek(446); f.write(struct.pack('<BBBBBBBBII',0,0xFE,0xFF,0xFF,0x0C,0xFE,0xFF,0xFF,2048,129024)); f.seek(510); f.write(b'\x55\xAA'); f.close()"

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

run-iso: CoolFrost.iso
	qemu-system-x86_64 -m 512M -cdrom CoolFrost.iso \
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

%.o: %.c ${HEADERS}
	${CC} ${CFLAGS} -c $< -o $@

%.o: %.asm
	nasm $< -f elf64 -o $@

clean:
	rm -rf *.iso *.dis *.o *.elf hdd.img
	rm -rf kernel/*.o boot/*.o boot/*.bin drivers/*.o cpu/*.o libc/*.o power/*.o vm/*.o net/*.o user/*.o
	rm -rf userland/*.elf
	rm -rf iso/
