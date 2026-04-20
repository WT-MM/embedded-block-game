ifneq (${KERNELRELEASE},)

# Compiled as part of the kernel build.
        obj-m := voxel_gpu.o

else

# Standalone build: invoke the kernel build system.

        KERNEL_SOURCE := /usr/src/linux-headers-$(shell uname -r)
        KERNEL_SOURCE_LAPTOP := /lib/modules/$(shell uname -r)/build
        PWD := $(shell pwd)

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra

default: module tests/voxel_test tests/renderer_scene_test tests/renderer_static_test tests/renderer_quad_test tests/renderer_edge_test game
laptop: module_laptop tests/voxel_test tests/renderer_scene_test tests/renderer_static_test tests/renderer_quad_test tests/renderer_edge_test game

module:
	${MAKE} -C ${KERNEL_SOURCE} SUBDIRS=${PWD} modules

module_laptop:
	${MAKE} -C ${KERNEL_SOURCE_LAPTOP} M=${PWD} modules

tests/voxel_test: tests/voxel_test.c voxel_gpu.h
	${CC} ${CFLAGS} -I. -o $@ tests/voxel_test.c

tests/renderer_scene_test: tests/renderer_scene_test.c renderer.c renderer.h voxel_gpu.h gpu_transport.c gpu_transport.h virtual_gpu_protocol.h block_types.h block_types.c world.c world.h
	${CC} ${CFLAGS} -I. -o $@ tests/renderer_scene_test.c renderer.c gpu_transport.c block_types.c world.c -lm

tests/renderer_static_test: tests/renderer_static_test.c renderer.c renderer.h voxel_gpu.h gpu_transport.c gpu_transport.h virtual_gpu_protocol.h block_types.h block_types.c world.c world.h
	${CC} ${CFLAGS} -I. -o $@ tests/renderer_static_test.c renderer.c gpu_transport.c block_types.c world.c -lm

tests/renderer_quad_test: tests/renderer_quad_test.c renderer.c renderer.h voxel_gpu.h gpu_transport.c gpu_transport.h virtual_gpu_protocol.h block_types.h block_types.c world.c world.h
	${CC} ${CFLAGS} -I. -o $@ tests/renderer_quad_test.c renderer.c gpu_transport.c block_types.c world.c -lm

tests/renderer_edge_test: tests/renderer_edge_test.c renderer.c renderer.h voxel_gpu.h gpu_transport.c gpu_transport.h virtual_gpu_protocol.h block_types.h block_types.c world.c world.h
	${CC} ${CFLAGS} -I. -o $@ tests/renderer_edge_test.c renderer.c gpu_transport.c block_types.c world.c -lm

game: game.c input.c input.h renderer.c renderer.h voxel_gpu.h gpu_transport.c gpu_transport.h virtual_gpu_protocol.h block_types.h block_types.c world.c world.h
	${CC} ${CFLAGS} -o $@ game.c input.c renderer.c gpu_transport.c block_types.c world.c -lm

laptop_run:
	VOXEL_MOUSE_INVERT_Y=1 VOXEL_GPU_BACKEND=socket ./game

clean:
	${MAKE} -C ${KERNEL_SOURCE} SUBDIRS=${PWD} clean
	${RM} tests/voxel_test tests/renderer_scene_test tests/renderer_static_test tests/renderer_quad_test tests/renderer_edge_test game

clean_laptop:
	${MAKE} -C ${KERNEL_SOURCE_LAPTOP} M=${PWD} clean
	${RM} tests/voxel_test tests/renderer_scene_test tests/renderer_static_test tests/renderer_quad_test tests/renderer_edge_test game

TARFILES = Makefile README voxel_gpu.h voxel_gpu.c virtual_gpu_protocol.h gpu_transport.h gpu_transport.c tests/voxel_test.c tests/renderer_scene_test.c tests/renderer_static_test.c tests/renderer_quad_test.c tests/renderer_edge_test.c renderer.c renderer.h block_types.c block_types.h world.c world.h
TARFILE  = sw.tar.gz
.PHONY: tar
tar: $(TARFILE)

$(TARFILE): $(TARFILES)
	tar zcfC $(TARFILE) .. $(TARFILES:%=sw/%)

endif
