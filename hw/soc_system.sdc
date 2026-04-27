#
# soc_system.sdc — Top-level timing constraints for the DE1-SoC voxel_gpu build.
#
# History:
#   * Prior versions only declared base clocks and let derive_pll_clocks figure
#     out the rest, which left every SDRAM / VGA I/O path completely
#     unconstrained.  That worked as long as Quartus happened to place the
#     SDRAM command+data registers in cells whose pad-to-pin delay was well
#     under one clock period, but it meant that every re-synthesis could
#     silently shift a critical path onto a slower route.  That is the most
#     likely explanation for the "chromatic aberration + left-edge zipper"
#     artifacts that reappeared after the SDRAM merge even though the RTL
#     change set was small: the RBF bitstream changed (3b2b01d vs 258a42d),
#     critical SDRAM read/write windows moved slightly, and read data started
#     being captured with marginal / violated setup or hold.
#   * This file now pins down:
#       1. Base board clocks.
#       2. PLL-derived clocks (via derive_pll_clocks).
#       3. SDRAM source-synchronous I/O timing relative to DRAM_CLK so that
#          the command/address/data paths are closed against the ISSI
#          IS42S16320D-7TL datasheet at 100 MHz.
#       4. 50 MHz <-> 100 MHz dcfifo crossings explicitly marked asynchronous.
#
# If Quartus reports "No paths constrained" after adding this file, the most
# common cause is the Altera PLL hierarchy pattern not matching — grep the
# compile report for `sdram_pll0` and adjust the wildcard below.
#

# ----------------------------------------------------------------------------
# 1. Base board clocks
# ----------------------------------------------------------------------------

foreach {clock port} {
    clock_50_1 CLOCK_50
    clock_50_2 CLOCK2_50
    clock_50_3 CLOCK3_50
    clock_50_4 CLOCK4_50
} {
    create_clock -name $clock -period 20ns [get_ports $port]
}

create_clock -name clock_27_1 -period 37 [get_ports TD_CLK27]

# ----------------------------------------------------------------------------
# 2. PLL-derived clocks
# ----------------------------------------------------------------------------
#
# sdram_pll0 produces:
#   outclk_0 : 100 MHz, phase  0   — internal SDRAM controller clock ("CLK")
#   outclk_1 : 100 MHz, phase -3ns — external SDRAM clock, drives DRAM_CLK pin
#
# derive_pll_clocks -create_base_clocks picks these up automatically.
#
derive_pll_clocks -create_base_clocks
derive_clock_uncertainty

# ----------------------------------------------------------------------------
# 3. SDRAM source-synchronous I/O timing
# ----------------------------------------------------------------------------
#
# ISSI IS42S16320D-7TL timing (the part populated on the DE1-SoC) at 100 MHz,
# CL=2 mode that the Sdram_Control IP configures it in:
#
#     tAC (clock -> valid data out) max = 5.4  ns
#     tOH (output hold after clock) min = 2.5  ns
#     tIS (input setup)             min = 1.5  ns
#     tIH (input hold)              min = 0.8  ns
#
# Board trace delay on DE1-SoC SDRAM lanes is ~0.5 ns one-way.  Combining
# datasheet and board delays (Terasic-recommended values that are known to
# close timing on DE1-SoC reference designs):
#
#     set_input_delay  -max 5.9  -min 3.0    (tAC+trace, tOH+trace)
#     set_output_delay -max 1.6  -min -0.9   (tIS+trace, -(tIH+trace))
#
# Use a virtual external clock for SDRAM I/O timing.  Binding a generated
# clock directly to the DRAM_CLK output port via the internal PLL hierarchy
# made Quartus 21.1's synthesis timing estimator crash during quartus_map on
# the lab machines.  A virtual clock still constrains the SDRAM data/command
# I/O windows without making Analysis & Synthesis depend on fragile generated
# PLL pin names.
create_clock -name sdram_clk_ext -period 10.000

set _sdram_inputs  [get_ports {DRAM_DQ[*]}]
set _sdram_outputs [get_ports { \
    DRAM_DQ[*] \
    DRAM_ADDR[*] \
    DRAM_BA[*] \
    DRAM_CAS_N \
    DRAM_CKE \
    DRAM_CS_N \
    DRAM_LDQM \
    DRAM_RAS_N \
    DRAM_UDQM \
    DRAM_WE_N \
}]

set_input_delay  -clock sdram_clk_ext -max 5.9 $_sdram_inputs
set_input_delay  -clock sdram_clk_ext -min 3.0 $_sdram_inputs

set_output_delay -clock sdram_clk_ext -max 1.6  $_sdram_outputs
set_output_delay -clock sdram_clk_ext -min -0.9 $_sdram_outputs

# ----------------------------------------------------------------------------
# 4. Clock domain crossings
# ----------------------------------------------------------------------------
#
# The 50 MHz system clock (clock_50_1 = CLOCK_50) and the SDRAM PLL output
# clocks only communicate through:
#
#   * Altera dcfifo macros (Sdram_WR_FIFO, Sdram_RD_FIFO), whose gray-code
#     pointer synchronizers must NOT be timed by STA.
#   * The sdram_ctrl_reset_n release, which is a static, multi-millisecond
#     sample that does not need to be timed.
#
# Declaring them async prevents Quartus from fabricating setup/hold failures
# on the gray pointer paths.  Keep both the internal 100 MHz controller clock
# and the generated external DRAM_CLK in this group; the FIFO crossings are
# between clock_50_1 and the internal PLL output, while the I/O constraints
# above are relative to sdram_clk_ext.
#
# clock_50_2/3/4 and clock_27_1 are not connected to the voxel_gpu SDRAM logic
# in this build, but we include them so any future use remains async-safe by
# default.
#
set _fabric_clocks [get_clocks {clock_50_1 clock_50_2 clock_50_3 clock_50_4 clock_27_1}]
set _sdram_clocks  [get_clocks -nowarn {sdram_clk_ext *sdram_pll0*}]

if {[llength $_sdram_clocks] > 0} {
    set_clock_groups -asynchronous \
        -group $_fabric_clocks \
        -group $_sdram_clocks
} else {
    post_message -type warning \
        "soc_system.sdc: no SDRAM PLL clocks found for async CDC cut; check TimeQuest report_clocks output."
}
