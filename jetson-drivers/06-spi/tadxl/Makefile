obj-m := tadxl.o tspi_sim.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all: modules spi_loop

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

spi_loop: spi_loop.c
	$(CC) -O2 -Wall -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f spi_loop
