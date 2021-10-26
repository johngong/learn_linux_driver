KVER = $(shell uname -r)
KDIR := /lib/modules/$(KVER)/build

obj-m = loop_dev.o
all:
	$(MAKE) -C $(KDIR) M=$(shell pwd) BUILD_INIT=m

clean:
	rm -f *.o *.ko .*.cmd *.mod.c .*.d .depend *~ Modules.symvers \
		Module.symvers Module.markers modules.order
	rm -rf .tmp_versions
