# Comment/uncomment the following line to disable/enable debugging
# DEBUG = y

ifeq ($(DEBUG),y)
	DEBFLAGS = -O -g -DSCULLVM_DEBUG # "-O" is needed to expand inlines
else
	DEBFLAGS = -O2
endif

#EXTRA_CFLAGS += $(DEBFLAGS) -I$(LLDINC)
#ccflags-y += $(DEBFLAGS) -I$(LLDINC)

TARGET = scullvm

ifneq ($(KERNELRELEASE),)

obj-m := scullvm.o

scullvm-y := main.o mmap.o

else

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
PWD	  := $(shell pwd)

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) LDDINC=$(PWD) modules

endif

install:
	install -d $(INSTALLDIR)
	install -c $(TARGET).o $(INSTALLDIR)

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions

depend .depend dep:
	$(CC) $(EXTRA_CFLAGS) -M *.c > .depend

ifeq (.depend,$(wildcard .depend))
include .depend
endif