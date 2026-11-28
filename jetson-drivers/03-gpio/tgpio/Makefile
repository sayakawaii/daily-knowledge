obj-m := tgpio.o tgpio_simboard.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all: modules wait_press

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

wait_press: wait_press.c
	$(CC) -O2 -Wall -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f wait_press
