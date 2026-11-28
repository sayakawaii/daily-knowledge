obj-m := tlm75.o
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
	[ -f ../../tools/i2c-stub/i2c-stub.ko ] || ../../tools/i2c-stub/build.sh

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
