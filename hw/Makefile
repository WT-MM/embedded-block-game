SYSTEM = soc_system

# Optional fixed Quartus 19.1 install root. If these paths do not exist on a
# given lab machine, the *19 targets fall back to the matching tools on PATH.
QUARTUS_19_ROOT ?= /tools/intel/intelFPGA/19.1
QUARTUS_19_BIN ?= $(QUARTUS_19_ROOT)/quartus/bin
QSYS_19_BIN ?= $(QUARTUS_19_ROOT)/quartus/sopc_builder/bin

ifneq ($(wildcard $(QSYS_19_BIN)/qsys-generate),)
QSYS_GENERATE_19 = $(QSYS_19_BIN)/qsys-generate
else
QSYS_GENERATE_19 = qsys-generate
endif

ifneq ($(wildcard $(QUARTUS_19_BIN)/quartus_sh),)
QUARTUS_SH_19 = $(QUARTUS_19_BIN)/quartus_sh
QUARTUS_MAP_19 = $(QUARTUS_19_BIN)/quartus_map
QUARTUS_STA_19 = $(QUARTUS_19_BIN)/quartus_sta
QUARTUS_CPF_19 = $(QUARTUS_19_BIN)/quartus_cpf
else
QUARTUS_SH_19 = quartus_sh
QUARTUS_MAP_19 = quartus_map
QUARTUS_STA_19 = quartus_sta
QUARTUS_CPF_19 = quartus_cpf
endif

QSYS_GENERATE ?= qsys-generate
QUARTUS_SH ?= quartus_sh
QUARTUS_MAP ?= quartus_map
QUARTUS_STA ?= quartus_sta
QUARTUS_CPF ?= quartus_cpf

TCL = $(SYSTEM).tcl
QSYS = $(SYSTEM).qsys
SOPCINFO = $(SYSTEM).sopcinfo
QIP = $(SYSTEM)/synthesis/$(SYSTEM).qip
HPS_PIN_TCL = $(SYSTEM)/synthesis/submodules/hps_sdram_p0_pin_assignments.tcl
HPS_PIN_MAP = hps_sdram_p0_all_pins.txt
QSYS_STAMP = $(SYSTEM)/synthesis/.qsys-stamp
PROJECT_STAMP = output_files/.project-stamp
QPF = $(SYSTEM).qpf
QSF = $(SYSTEM).qsf
SDC = $(SYSTEM).sdc
SRF = $(SYSTEM).srf

BOARD_INFO = $(SYSTEM)_board_info.xml
DTS = $(SYSTEM).dts
DTB = $(SYSTEM).dtb

SOF = output_files/$(SYSTEM).sof
RBF = output_files/$(SYSTEM).rbf
RBF_INSTALL_SRC ?= ./$(SYSTEM).rbf
BOOT_DEV ?= /dev/mmcblk0p1
BOOT_MOUNT ?= /mnt

SOFTWARE_DIR = software

BSP_DIR = $(SOFTWARE_DIR)/spl_bsp
BSP_SETTINGS = $(BSP_DIR)/settings.bsp
PRELOADER_SETTINGS_DIR = hps_isw_handoff/soc_system_hps_0
PRELOADER_MAKEFILE = $(BSP_DIR)/Makefile
PRELOADER_MKPIMAGE = $(BSP_DIR)/preloader-mkpimage.bin

UBOOT_IMAGE = $(BSP_DIR)/uboot-socfpga/u-boot.img

# We had used Git branch "socfpga-4.19", but that disappeared
# Tag v4.19 seems to be the final version of the 4.19 kernel

KERNEL_REPO = https://github.com/altera-fpga/linux-socfpga.git
KERNEL_TAG = v4.19
DEFAULT_CONFIG = socfpga_defconfig
CROSS = env CROSS_COMPILE=arm-altera-eabi- ARCH=arm

KERNEL_DIR = $(SOFTWARE_DIR)/linux-socfpga
KERNEL_CONFIG = $(KERNEL_DIR)/.config
ZIMAGE = $(KERNEL_DIR)/arch/arm/boot/zImage

VOXEL_GPU_RTL = $(wildcard voxel_gpu/rtl/*.sv)
VOXEL_GPU_ASSETS = voxel_gpu/assets/textures.mif voxel_gpu/assets/recip_lut.hex
VOXEL_GPU_DEBUG_ASSETS = voxel_gpu/assets/textures_preview.png
VOXEL_GPU_SCRIPTS = voxel_gpu/scripts/generate_textures.py voxel_gpu/scripts/generate_recip_lut.py

TARFILES = Makefile \
	$(TCL) \
	$(QSYS) \
	$(SYSTEM)_top.sv \
	$(BOARD_INFO) \
	$(VOXEL_GPU_RTL) \
	$(VOXEL_GPU_ASSETS) \
	$(VOXEL_GPU_DEBUG_ASSETS) \
	$(VOXEL_GPU_SCRIPTS) \
	vga_ball.sv

TARFILE = lab3-hw.tar.gz

# project
#
# Run the topmost tcl script to generate the initial project files
#
# This also adds the pin constraints for the sdram subsystem, which
# Platform Designer places in in the HPS_PIN_TCL file.
#
# However, to run that, quartus_map has to run once to prepare the netlist
# enough so that the HPS_PIN_TCL script can run

.PHONY : project
project : $(PROJECT_STAMP)

$(PROJECT_STAMP) : $(TCL) $(QSYS_STAMP)
	$(QUARTUS_SH) -t $(TCL)
	$(QUARTUS_MAP) $(SYSTEM)
	$(QUARTUS_STA) -t $(HPS_PIN_TCL) $(SYSTEM)
	mkdir -p $(dir $@)
	touch $@

.PHONY : project19
project19 : QUARTUS_SH=$(QUARTUS_SH_19)
project19 : QUARTUS_MAP=$(QUARTUS_MAP_19)
project19 : QUARTUS_STA=$(QUARTUS_STA_19)
project19 : project

# qsys
#
# From the .qsys file, generate the .sopcinfo, .qip, and directory
# (named according to the system) with all the Verilog files, etc.

.PHONY : qsys
qsys : $(QSYS_STAMP)

QSYS_COMPONENT_SOURCES = voxel_gpu_hw.tcl $(VOXEL_GPU_RTL) $(VOXEL_GPU_ASSETS) \
	$(VOXEL_GPU_SCRIPTS) vga_ball.sv vga_ball_hw.tcl \
	$(wildcard sdram_local_test/*.v) \
	$(wildcard sdram_local_test/*.h) \
	$(wildcard sdram_local_test/sdram_pll0/*.v)
TOP_LEVEL_SOURCES = $(SYSTEM)_top.sv

$(QSYS_STAMP) : $(QSYS) $(QSYS_COMPONENT_SOURCES)
	rm -rf $(SOPCINFO) $(SYSTEM)/
	$(QSYS_GENERATE) $(QSYS) --synthesis=VERILOG
	test -f $(SOPCINFO)
	test -f $(QIP)
	test -f $(HPS_PIN_TCL)
	touch $@

.PHONY : qsys19
qsys19 : QSYS_GENERATE=$(QSYS_GENERATE_19)
qsys19 : qsys

# quartus
#
# Run Quartus on the Qsys-generated files
#
# Note that for this to succeed, quartus_map has to be run once
# on the project so that quartus_sta can be run on the HPS_PIN_TCL
# script to set up the proper constraints for all the SDRAM pins

.PHONY : quartus
quartus : $(SOF)

$(SOF) $(HPS_PIN_MAP) : $(PROJECT_STAMP) $(TOP_LEVEL_SOURCES)
	$(QUARTUS_SH) --flow compile $(QPF)


# rbf
#
# Convert the .sof file (for programming through the USB blaster)
# to an .rbf file to be placed on an SD card and written by u-boot
.PHONY : rbf
rbf : $(RBF)

$(RBF) : $(SOF)
	$(QUARTUS_CPF) -c $(SOF) $(RBF)

.PHONY : print-quartus19-tools
print-quartus19-tools :
	@echo "QUARTUS_19_ROOT=$(QUARTUS_19_ROOT)"
	@echo "QUARTUS_19_BIN=$(QUARTUS_19_BIN)"
	@echo "QSYS_19_BIN=$(QSYS_19_BIN)"
	@echo "QSYS_GENERATE_19=$(QSYS_GENERATE_19)"
	@echo "QUARTUS_SH_19=$(QUARTUS_SH_19)"
	@echo "QUARTUS_MAP_19=$(QUARTUS_MAP_19)"
	@echo "QUARTUS_STA_19=$(QUARTUS_STA_19)"
	@echo "QUARTUS_CPF_19=$(QUARTUS_CPF_19)"

# install-rbf
#
# Copy an already-built .rbf into the mounted SD boot partition and flush it.
# By default this uses ./$(SYSTEM).rbf (the copy in the hw/ working directory);
# override RBF_INSTALL_SRC if you want to install a different .rbf (e.g. the
# freshly built one under output_files/).
# Use mount-boot first if the boot partition is not yet mounted.
.PHONY : mount-boot install-rbf install-built-rbf
mount-boot :
	mkdir -p $(BOOT_MOUNT)
	mount | grep -q " $(BOOT_MOUNT) " || mount $(BOOT_DEV) $(BOOT_MOUNT)

install-rbf :
	@test -f "$(RBF_INSTALL_SRC)" || (echo "missing $(RBF_INSTALL_SRC)"; exit 1)
	@test -d "$(BOOT_MOUNT)" || (echo "$(BOOT_MOUNT) does not exist; run 'make mount-boot' first"; exit 1)
	@mount | grep -q " $(BOOT_MOUNT) " || (echo "$(BOOT_MOUNT) is not mounted; run 'make mount-boot' first"; exit 1)
	cp $(RBF_INSTALL_SRC) $(BOOT_MOUNT)/$(SYSTEM).rbf
	sync
	ls -lh $(BOOT_MOUNT)/$(SYSTEM).rbf

install-built-rbf : $(RBF)
	@test -d "$(BOOT_MOUNT)" || (echo "$(BOOT_MOUNT) does not exist; run 'make mount-boot' first"; exit 1)
	@mount | grep -q " $(BOOT_MOUNT) " || (echo "$(BOOT_MOUNT) is not mounted; run 'make mount-boot' first"; exit 1)
	cp $(RBF) $(BOOT_MOUNT)/$(SYSTEM).rbf
	sync
	ls -lh $(BOOT_MOUNT)/$(SYSTEM).rbf

# dtb
#
# Use the .sopcinfo file to generate a device tree blob file
# with information about the memory map of the peripherals
.PHONY : dtb
dtb : $(DTB)

$(DTB) : $(DTS)
	@which dtc || (echo "dtc not found.  Did you run embedded_command_shell.sh?"; exit 1)
	dtc -I dts -O dtb -o $(DTB) $(DTS)

$(DTS) : $(SOPCINFO) $(BOARD_INFO)
	@which sopc2dts || (echo "sopc2dts not found.  Did you run embedded_command_shell.sh?"; exit 1)
	sopc2dts --input $(SOPCINFO) \
		--output $(DTS) \
		--type dts \
		--board $(BOARD_INFO) \
		--clocks

# preloader
#
# Builds the SPL's preloader-mkpimage.bin image file, which should
# be written to the "magic" 3rd parition on the SD card
# in software/spl_bsp
#
# Requires the embedded_command_shell.sh script to have run so the compiler,
# etc is available
#
.PHONY : preloader
preloader : $(PRELOADER_MKPIMAGE)

$(PRELOADER_MKPIMAGE) : $(PRELOADER_MAKEFILE) $(BSP_SETTINGS)
	$(MAKE) -C $(BSP_DIR)

$(BSP_SETTINGS) $(PRELOADER_MAKEFILE) : $(PRELOADER_SETTINGS_DIR)
	mkdir -p $(BSP_DIR)
	bsp-create-settings \
	  --type spl \
	  --bsp-dir $(BSP_DIR) \
	  --settings $(BSP_SETTINGS) \
	  --preloader-settings-dir $(PRELOADER_SETTINGS_DIR) \
	  --set spl.boot.FAT_SUPPORT 1

# uboot
#
# Build the bootloader

.PHONY : uboot
uboot : $(UBOOT_IMAGE)

$(UBOOT_IMAGE) : $(PRELOADER_MAKEFILE) $(BSP_SETTINGS)
	$(MAKE) -C $(BSP_DIR) uboot

# kernel-download
#
#   Clone the Linux kernel repository
#
# kernel-config
#
#   Set up the default kernel configuration
#
# kernel-menuconfig
#
#   (Optional) Access the kernel configuration menu to make further
#   adjustments about which modules are included
#
# zimage
#
#   Compile the kernel

.PHONY : download-kernel config-kernel zimage
kernel-download : $(KERNEL_DIR)
kernel-config : $(KERNEL_CONFIG)
kernel-menuconfig :
	$(CROSS) $(MAKE) -C $(KERNEL_DIR) menuconfig
zimage : $(ZIMAGE)

$(KERNEL_DIR) :
	mkdir -p $(KERNEL_DIR)
	git clone --branch $(KERNEL_TAG) --single-branch --depth 1 \
		$(KERNEL_REPO) $(KERNEL_DIR)

# Configure the kernel.  Start from a provided default,
# 
# Turn off version checking (makes it easier to compile kernel
# modules and not have them complain about version)
#
# Turn on large file (+2TB) support, which the ext4 filesystem
# requires by default (it will not be able to mount the root
# filesystem read/write otherwise)
$(KERNEL_CONFIG) : $(KERNEL_DIR)
	$(CROSS) $(MAKE) -C $(KERNEL_DIR) $(DEFAULT_CONFIG)
	$(KERNEL_DIR)/scripts/config --file $(KERNEL_CONFIG) \
	  --disable CONFIG_LOCALVERSION_AUTO \
	  --enable CONFIG_LBDAF \
	  --disable CONFIG_XFS_FS \
	  --disable CONFIG_GFS2_FS \
	  --disable CONFIG_TEST_KMOD

# Compile the kernel

$(ZIMAGE) : $(KERNEL_CONFIG)
	$(CROSS) $(MAKE) -C $(KERNEL_DIR) LOCALVERSION= zImage

# tar
#
# Build soc_system.tar.gz

.PHONY : tar
tar : $(TARFILE)

$(TARFILE) : $(TARFILES)
	tar zcfC $(TARFILE) .. $(TARFILES:%=lab3-hw/%)

# clean
#
# Remove all generated files

.PHONY : clean quartus-clean qsys-clean project-clean
clean : quartus-clean qsys-clean project-clean dtb-clean preloader-clean \
	uboot-clean

project-clean :
	rm -rf $(QPF) $(QSF) $(SDC)

qsys-clean :
	rm -rf $(SOPCINFO) $(QIP) $(SYSTEM)/ .qsys_edit \
	hps_isw_handoff/ hps_sdram_p0_summary.csv

quartus-clean :
	rm -rf  $(SOF) output_files db incremental_db $(SYSTEM).qdf \
	c5_pin_model_dump.txt $(HPS_PIN_MAP)

dtb-clean :
	rm -rf $(DTS) $(DTB)

preloader-clean :
	rm -rf $(BSP_DIR)

uboot-clean :
	rm -rf $(BSP_DIR)/uboot-socfpga

kernel-clean :
	rm -rf $(KERNEL_DIR)

config-clean :
	rm -rf $(KERNEL_CONFIG)
