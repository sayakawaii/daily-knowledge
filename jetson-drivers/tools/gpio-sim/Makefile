obj-m := gpio-sim-ext.o
gpio-sim-ext-y := gpio-sim.o irq_sim.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f gpio-sim.c irq_sim.c
