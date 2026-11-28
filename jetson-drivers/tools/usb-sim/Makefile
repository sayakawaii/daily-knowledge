obj-m += dummy_hcd.o usb_f_ss_lb.o
usb_f_ss_lb-y := f_loopback.o f_sourcesink.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
