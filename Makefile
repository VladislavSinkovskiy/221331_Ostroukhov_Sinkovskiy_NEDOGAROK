MODULE_NAME := cryptfs

obj-m += $(MODULE_NAME).o
$(MODULE_NAME)-objs := cryptfs_main.o cryptfs_super.o cryptfs_inode.o \
                       cryptfs_file.o cryptfs_crypto.o cryptfs_ctl.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

USERSPACE := cryptfs-ctl

.PHONY: all modules clean userspace

all: modules userspace

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

userspace: $(USERSPACE)

$(USERSPACE): cryptfs-ctl.c cryptfs_uapi.h
	$(CC) -Wall -O2 -o $@ cryptfs-ctl.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(USERSPACE)
