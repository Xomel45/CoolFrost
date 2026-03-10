C_SOURCES = $(wildcard kernel/*.c drivers/*.c cpu/*.c libc/*.c power/*.c fs/*.c)
HEADERS   = $(wildcard kernel/*.h drivers/*.h cpu/*.h libc/*.h power/*.h fs/*.h)
OBJ       = ${C_SOURCES:.c=.o} cpu/interrupt.o drivers/cpuid-detect.o

CC     = x86_64-elf-gcc
LD     = x86_64-elf-ld
GDB    = gdb

# 64-bit freestanding kernel flags
# -mno-red-zone: disable the 128-byte red zone (mandatory for interrupt handlers)
# -fno-pic:      no position-independent code (we link at a fixed address)
CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions \
         -m64 -mno-red-zone -fno-pic -fno-stack-protector

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
	rm -rf kernel/*.o boot/*.o drivers/*.o cpu/*.o libc/*.o power/*.o vm/*.o
	rm -rf iso/
