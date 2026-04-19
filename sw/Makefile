ifneq (${KERNELRELEASE},)

# Compiled as part of the kernel build.
        obj-m := voxel_gpu.o

else

# Standalone build: invoke the kernel build system.

        KERNEL_SOURCE := /usr/src/linux-headers-$(shell uname -r)
        PWD := $(shell pwd)

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra

default: module voxel_test renderer_scene_test renderer_static_test game

module:
	${MAKE} -C ${KERNEL_SOURCE} SUBDIRS=${PWD} modules

voxel_test: voxel_test.c voxel_gpu.h
	${CC} ${CFLAGS} -o $@ voxel_test.c

renderer_scene_test: renderer_scene_test.c renderer.c renderer.h voxel_gpu.h block_types.h block_types.c
	${CC} ${CFLAGS} -o $@ renderer_scene_test.c renderer.c block_types.c -lm

renderer_static_test: renderer_static_test.c renderer.c renderer.h voxel_gpu.h block_types.h block_types.c
	${CC} ${CFLAGS} -o $@ renderer_static_test.c renderer.c block_types.c -lm

game: game.c input.c input.h renderer.c renderer.h voxel_gpu.h block_types.h block_types.c
	${CC} ${CFLAGS} -o $@ game.c input.c renderer.c block_types.c -lm

clean:
	${MAKE} -C ${KERNEL_SOURCE} SUBDIRS=${PWD} clean
	${RM} voxel_test renderer_scene_test renderer_static_test game

TARFILES = Makefile README voxel_gpu.h voxel_gpu.c voxel_test.c renderer_scene_test.c renderer_static_test.c renderer.c renderer.h block_types.c block_types.h
TARFILE  = sw.tar.gz
.PHONY: tar
tar: $(TARFILE)

$(TARFILE): $(TARFILES)
	tar zcfC $(TARFILE) .. $(TARFILES:%=sw/%)

endif
