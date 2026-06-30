# CC      ?= cc
# CFLAGS  ?= -O2 -Wall -Wextra
#
# mchbar_read: mchbar_read.c mchbar_regs.h
# 	$(CC) $(CFLAGS) -o $@ mchbar_read.c
#
# .PHONY: clean
# clean:
# 	rm -f mchbar_read

obj-m += mchbar_kmod.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# convenience targets
load: all
	sudo insmod mchbar_kmod.ko
	@echo "==> see /sys/kernel/debug/mchbar/ (root required)"

unload:
	sudo rmmod mchbar_kmod

dump:
	sudo cat /sys/kernel/debug/mchbar/registers

.PHONY: all clean load unload dump
