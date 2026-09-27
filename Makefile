CC      = gcc
CFLAGS  = -m32 -ffreestanding -fno-pic -fno-stack-protector -nostdlib -O2 -Wall -Wextra
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib --no-warn-rwx-segments

kernel.elf: kernel.c font.h linker.ld
	$(CC) $(CFLAGS) -c kernel.c -o kernel.o
	ld $(LDFLAGS) kernel.o -o kernel.elf

run: kernel.elf
	qemu-system-x86_64 -enable-kvm -kernel kernel.elf -serial stdio

run-nokvm: kernel.elf
	qemu-system-x86_64 -kernel kernel.elf -nographic

clean:
	rm -f kernel.o kernel.elf
