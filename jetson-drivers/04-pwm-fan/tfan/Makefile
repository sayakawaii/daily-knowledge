obj-m := tfan.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
	$(MAKE) -C ../../tools/regpeek

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
