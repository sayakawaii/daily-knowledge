obj-m := tvcan.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all: modules can_echo

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

can_echo: can_echo.c
	$(CC) -O2 -Wall -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f can_echo
