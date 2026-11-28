obj-m += tcam.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all: cam_capture
	$(MAKE) -C $(KDIR) M=$(PWD) modules

cam_capture: cam_capture.c
	gcc -O2 -Wall -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f cam_capture
