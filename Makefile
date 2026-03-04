C_SOURCES = $(wildcard kernel/*.c drivers/*.c cpu/*.c libc/*.c power/*.c)
HEADERS = $(wildcard kernel/*.h drivers/*.h cpu/*.h libc/*.h power/*.h)
OBJ = ${C_SOURCES:.c=.o cpu/interrupt.o drivers/cpuid-detect.o }
CC = i386-elf-gcc
GDB = gdb
CFLAGS = -g -ffreestanding -Wall -Wextra -fno-exceptions -m32

kernel.elf: boot/multiboot_entry.o ${OBJ}
	i386-elf-ld -o $@ -T linker.ld $^

kernel.bin: boot/kernel_entry.o ${OBJ}
	i386-elf-ld -o $@ -Ttext 0x1000 $^ --oformat binary

#MADE FOR LOCAL USE, I AM NOT RESPONSIBLE IF SOMETHING HAPPENS
#usb: kernel.elf
#	sudo mkdir -p /mnt/usb
#	sudo mount /dev/sdd1 /mnt/usb
#	sudo mkdir -p /mnt/usb/boot/grub
#	sudo cp kernel.elf /mnt/usb/boot/kernel.bin
#	sudo cp grub.cfg /mnt/usb/boot/grub/
#	sudo umount /mnt/usb
#	@echo "USB ready! Boot from it now."

os-image.bin: temp-image.bin
	dd if=/dev/zero of=$@ bs=1M count=4
	dd if=temp-image.bin of=$@ conv=notrunc
	dd if=/dev/zero of=hdd.img bs=1M count=4

temp-image.bin: boot/bootsect.bin kernel.bin
	cat $^ > temp-image.bin

run: os-image.bin
	qemu-system-i386 -m 512M -drive file=os-image.bin,format=raw,if=floppy -drive file=hdd.img,format=raw,if=ide -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -enable-kvm -cpu host -vga std

run-grub: kernel.elf
	qemu-system-i386 -m 512M -kernel kernel.elf -drive file=hdd.img,format=raw,if=ide -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -enable-kvm -cpu host -vga qxl

iso: kernel.elf
	mkdir -p ./iso/boot/grub/
	cp ./grub.cfg ./iso/boot/grub/grub.cfg
	cp ./kernel.elf ./iso/boot/kernel.bin
	grub-mkrescue -o FrostOS.iso iso/

debug: os-image.bin kernel.elf
	qemu-system-i386 -S -s -m 512M -fda os-image.bin -hda hdd.img -audiodev pa,id=speaker -machine pcspk-audiodev=speaker -d guest_errors,int &
	${GDB} -ex "target remote localhost:1234" -ex "symbol-file kernel.elf"

check: ${C_SOURCES}
	${CC} -ffreestanding -fsyntax-only -Wall -Wextra -m32 -c $^

%.o: %.c ${HEADERS}
	${CC} ${CFLAGS} -c $< -o $@

%.o: %.asm
	nasm $< -f elf -o $@

%.bin: %.asm
	nasm $< -f bin -o $@

clean:
	rm -rf *.bin *.dis *.o os-image.bin *.elf
	rm -rf kernel/*.o boot/*.bin drivers/*.o boot/*.o cpu/*.o libc/*.o vm/*.o
	rm -rf iso/
