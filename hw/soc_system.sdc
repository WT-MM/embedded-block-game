
    foreach {clock port} {
	clock_50_1 CLOCK_50
	clock_50_2 CLOCK2_50
	clock_50_3 CLOCK3_50
	clock_50_4 CLOCK4_50
    } {
	create_clock -name $clock -period 20ns [get_ports $port]
    }
    
    create_clock -name clock_27_1 -period 37 [get_ports TD_CLK27]

    derive_pll_clocks -create_base_clocks
    derive_clock_uncertainty

    # The voxel SDRAM controller crosses between the Qsys 50 MHz fabric
    # clock and its local 100 MHz SDRAM PLL using DCFIFOs plus explicit
    # load/config handshakes. Treat that boundary as CDC for timing.
    set qsys_50_clk [get_clocks -nowarn {*sdram_clocks*sys_pll*general*0*gpll~PLL_OUTPUT_COUNTER|divclk}]
    set voxel_sdram_100_clk [get_clocks -nowarn {*voxel_gpu_0*sdram_ctrl*sdram_pll0_inst*general*0*gpll~PLL_OUTPUT_COUNTER|divclk}]
    if {[get_collection_size $qsys_50_clk] > 0 && [get_collection_size $voxel_sdram_100_clk] > 0} {
        set_clock_groups -asynchronous -group $qsys_50_clk -group $voxel_sdram_100_clk
    }
