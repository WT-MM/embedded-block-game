ifneq (${KERNELRELEASE},)

# Compiled as part of the kernel build.
        obj-m := voxel_gpu.o

else

# Standalone build: invoke the kernel build system.

        KERNEL_SOURCE := /usr/src/linux-headers-$(shell uname -r)
        PWD := $(shell pwd)

        CC      ?= gcc
        CFLAGS  ?= -O2 -Wall -Wextra

default: module voxel_test

module:
	${MAKE} -C ${KERNEL_SOURCE} SUBDIRS=${PWD} modules

voxel_test: voxel_test.c voxel_gpu.h
	${CC} ${CFLAGS} -o $@ voxel_test.c

clean:
	${MAKE} -C ${KERNEL_SOURCE} SUBDIRS=${PWD} clean
	${RM} voxel_test

TARFILES = Makefile README voxel_gpu.h voxel_gpu.c voxel_test.c
TARFILE  = sw.tar.gz
.PHONY: tar
tar: $(TARFILE)

$(TARFILE): $(TARFILES)
	tar zcfC $(TARFILE) .. $(TARFILES:%=sw/%)

endif
