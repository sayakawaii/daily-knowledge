obj-m += tusb.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all: usb_loop
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	../../tools/usb-sim/build.sh

usb_loop: usb_loop.c
	gcc -O2 -Wall -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f usb_loop
