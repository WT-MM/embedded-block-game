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
THREAD_FLAGS ?= -pthread

TEST_TARGETS := \
	tests/voxel_test \
	tests/command_parser_test \
	tests/inventory_test \
	tests/player_physics_test \
	tests/world_chunk_test \
	tests/renderer_scene_test \
	tests/renderer_static_test \
	tests/renderer_quad_test \
	tests/renderer_edge_test \
	tests/renderer_fog_test \
	tests/fpga_sdram_test \
	tests/fpga_band_offset_test

RENDERER_TEST_SRCS := renderer.c gpu_transport.c block_types.c world.c world_gen.c
RENDERER_TEST_DEPS := renderer.c renderer.h voxel_gpu.h \
	gpu_transport.c gpu_transport.h virtual_gpu_protocol.h \
	block_types.c block_types.h texture_tiles.def world.c world.h \
	world_gen.c world_gen.h env_util.h

.PHONY: default laptop tests module module_laptop laptop_run clean clean_laptop clean_outputs clean_tests tar

default: module tests game
laptop: module_laptop tests game
tests: $(TEST_TARGETS)

module:
	${MAKE} -C ${KERNEL_SOURCE} SUBDIRS=${PWD} modules

module_laptop:
	${MAKE} -C ${KERNEL_SOURCE_LAPTOP} M=${PWD} modules

tests/voxel_test: tests/voxel_test.c voxel_gpu.h
	${CC} ${CFLAGS} -I. -o $@ tests/voxel_test.c

tests/command_parser_test: tests/command_parser_test.c command_parser.c command_parser.h inventory.h block_types.h texture_tiles.def
	${CC} ${CFLAGS} -I. -o $@ tests/command_parser_test.c command_parser.c

tests/inventory_test: tests/inventory_test.c inventory.c inventory.h block_types.c block_types.h texture_tiles.def
	${CC} ${CFLAGS} -I. -o $@ tests/inventory_test.c inventory.c block_types.c

tests/player_physics_test: tests/player_physics_test.c player_physics.c player_physics.h world.c world.h world_gen.c world_gen.h env_util.h block_types.c block_types.h texture_tiles.def
	${CC} ${CFLAGS} ${THREAD_FLAGS} -I. -o $@ tests/player_physics_test.c player_physics.c world.c world_gen.c block_types.c -lm

tests/world_chunk_test: tests/world_chunk_test.c block_types.h block_types.c texture_tiles.def world.c world.h world_gen.c world_gen.h env_util.h
	${CC} ${CFLAGS} ${THREAD_FLAGS} -I. -o $@ tests/world_chunk_test.c block_types.c world.c world_gen.c -lm

tests/renderer_scene_test: tests/renderer_scene_test.c $(RENDERER_TEST_DEPS)
	${CC} ${CFLAGS} ${THREAD_FLAGS} -I. -o $@ $< $(RENDERER_TEST_SRCS) -lm

tests/renderer_static_test: tests/renderer_static_test.c $(RENDERER_TEST_DEPS)
	${CC} ${CFLAGS} ${THREAD_FLAGS} -I. -o $@ $< $(RENDERER_TEST_SRCS) -lm

tests/renderer_quad_test: tests/renderer_quad_test.c $(RENDERER_TEST_DEPS)
	${CC} ${CFLAGS} ${THREAD_FLAGS} -I. -o $@ $< $(RENDERER_TEST_SRCS) -lm

tests/renderer_edge_test: tests/renderer_edge_test.c $(RENDERER_TEST_DEPS)
	${CC} ${CFLAGS} ${THREAD_FLAGS} -I. -o $@ $< $(RENDERER_TEST_SRCS) -lm

tests/renderer_fog_test: tests/renderer_fog_test.c $(RENDERER_TEST_DEPS)
	${CC} ${CFLAGS} ${THREAD_FLAGS} -I. -o $@ $< $(RENDERER_TEST_SRCS) -lm

tests/fpga_sdram_test: tests/fpga_sdram_test.c
	${CC} ${CFLAGS} -I. -o $@ tests/fpga_sdram_test.c

tests/fpga_band_offset_test: tests/fpga_band_offset_test.c voxel_gpu.h
	${CC} ${CFLAGS} -I. -o $@ tests/fpga_band_offset_test.c

game: game.c game_home.c game_home.h game_items.c game_items.h player_physics.c player_physics.h input.c input.h renderer.c renderer.h voxel_gpu.h gpu_transport.c gpu_transport.h virtual_gpu_protocol.h block_types.h block_types.c texture_tiles.def world.c world.h world_gen.c world_gen.h mesh_worker.c mesh_worker.h gen_worker.c gen_worker.h thread_affinity.c thread_affinity.h chat.c chat.h command_parser.c command_parser.h pause_menu.c pause_menu.h inventory.c inventory.h env_util.h
	${CC} ${CFLAGS} ${THREAD_FLAGS} -o $@ game.c game_home.c game_items.c input.c renderer.c gpu_transport.c block_types.c world.c world_gen.c mesh_worker.c gen_worker.c thread_affinity.c player_physics.c chat.c command_parser.c pause_menu.c inventory.c -lm

laptop_run:
	VOXEL_GPU_BACKEND=socket ./game

clean: clean_outputs
	${MAKE} -C ${KERNEL_SOURCE} SUBDIRS=${PWD} clean

clean_laptop: clean_outputs
	${MAKE} -C ${KERNEL_SOURCE_LAPTOP} M=${PWD} clean

clean_outputs: clean_tests
	${RM} game

clean_tests:
	${RM} $(TEST_TARGETS)

TARFILES = Makefile README \
	voxel_gpu.h voxel_gpu.c virtual_gpu_protocol.h \
	gpu_transport.h gpu_transport.c renderer.c renderer.h \
	input.c input.h player_physics.c player_physics.h \
	game_home.c game_home.h game_items.c game_items.h \
	block_types.c block_types.h texture_tiles.def \
	world.c world.h world_gen.c world_gen.h env_util.h \
	mesh_worker.c mesh_worker.h gen_worker.c gen_worker.h \
	thread_affinity.c thread_affinity.h \
	chat.c chat.h command_parser.c command_parser.h \
	pause_menu.c pause_menu.h inventory.c inventory.h \
	tests/voxel_test.c tests/command_parser_test.c tests/inventory_test.c \
	tests/player_physics_test.c \
	tests/world_chunk_test.c tests/renderer_scene_test.c \
	tests/renderer_static_test.c tests/renderer_quad_test.c \
	tests/renderer_edge_test.c tests/renderer_fog_test.c \
	tests/fpga_sdram_test.c tests/fpga_band_offset_test.c
TARFILE  = sw.tar.gz
tar: $(TARFILE)

$(TARFILE): $(TARFILES)
	tar zcfC $(TARFILE) .. $(TARFILES:%=sw/%)

endif
