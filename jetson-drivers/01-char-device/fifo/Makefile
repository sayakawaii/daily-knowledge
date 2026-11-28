obj-m := fifo.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all: modules test_fifo

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

test_fifo: test_fifo.c fifo_ioctl.h
	gcc -Wall -O2 -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f test_fifo
