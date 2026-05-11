// Minimal simulation-only harness for voxel_gpu.
//
// This testbench drives the Avalon-MM CSR/FIFO front door and dumps a VCD.
// It is intentionally conservative: the board SDRAM pins are left as a dummy
// conduit, and meaningful raster/display timing still requires simulator
// support for the vendor RAM/FIFO/PLL primitives instantiated by the RTL.

`timescale 1ns/1ps

module voxel_gpu_tb;
    logic clk;
    logic reset;

    logic [12:0] address;
    logic        chipselect;
    logic        write;
    logic [31:0] writedata;
    logic  [3:0] byteenable;
    logic [31:0] readdata;

    logic [7:0] VGA_R;
    logic [7:0] VGA_G;
    logic [7:0] VGA_B;
    logic       VGA_CLK;
    logic       VGA_HS;
    logic       VGA_VS;
    logic       VGA_BLANK_n;
    logic       VGA_SYNC_n;

    logic [12:0] DRAM_ADDR;
    logic  [1:0] DRAM_BA;
    logic        DRAM_CAS_N;
    logic        DRAM_CKE;
    logic        DRAM_CLK;
    logic        DRAM_CS_N;
    wire  [15:0] DRAM_DQ;
    logic        DRAM_LDQM;
    logic        DRAM_RAS_N;
    logic        DRAM_UDQM;
    logic        DRAM_WE_N;

    voxel_gpu dut (
        .clk(clk),
        .reset(reset),
        .address(address),
        .chipselect(chipselect),
        .write(write),
        .writedata(writedata),
        .byteenable(byteenable),
        .readdata(readdata),
        .VGA_R(VGA_R),
        .VGA_G(VGA_G),
        .VGA_B(VGA_B),
        .VGA_CLK(VGA_CLK),
        .VGA_HS(VGA_HS),
        .VGA_VS(VGA_VS),
        .VGA_BLANK_n(VGA_BLANK_n),
        .VGA_SYNC_n(VGA_SYNC_n),
        .DRAM_ADDR(DRAM_ADDR),
        .DRAM_BA(DRAM_BA),
        .DRAM_CAS_N(DRAM_CAS_N),
        .DRAM_CKE(DRAM_CKE),
        .DRAM_CLK(DRAM_CLK),
        .DRAM_CS_N(DRAM_CS_N),
        .DRAM_DQ(DRAM_DQ),
        .DRAM_LDQM(DRAM_LDQM),
        .DRAM_RAS_N(DRAM_RAS_N),
        .DRAM_UDQM(DRAM_UDQM),
        .DRAM_WE_N(DRAM_WE_N)
    );

    initial clk = 1'b0;
    always #10 clk = ~clk; // 50 MHz.

    task automatic avalon_write(input logic [12:0] addr, input logic [31:0] data);
        begin
            @(negedge clk);
            address = addr;
            writedata = data;
            chipselect = 1'b1;
            write = 1'b1;
            byteenable = 4'hF;
            @(negedge clk);
            chipselect = 1'b0;
            write = 1'b0;
            address = 13'd0;
            writedata = 32'd0;
        end
    endtask

    initial begin
        string vcd_path;
        if (!$value$plusargs("vcd=%s", vcd_path)) begin
            vcd_path = "build/diagrams/voxel_gpu.vcd";
        end
        $dumpfile(vcd_path);
        $dumpvars(0, voxel_gpu_tb);

        reset = 1'b1;
        address = 13'd0;
        chipselect = 1'b0;
        write = 1'b0;
        writedata = 32'd0;
        byteenable = 4'h0;

        repeat (8) @(negedge clk);
        reset = 1'b0;
        repeat (4) @(negedge clk);

        // CONTROL.EN.
        avalon_write(13'h000, 32'h0000_0001);

        // Palette entry 0 = black; palette entry auto-increments in RTL.
        avalon_write(13'h003, 32'h0000_0000);
        avalon_write(13'h004, 32'h0000_0000);

        // Begin band 0 with full local-row window 0..59.
        avalon_write(13'h00D, 32'h0000_0000);
        avalon_write(13'h00F, {18'd0, 6'd59, 2'd0, 6'd0});
        avalon_write(13'h00E, 32'h0000_0001);

        // Push one safe flat-color 1x1 primer-like descriptor to the FIFO
        // window. Real raster validation should replace this with deliberate
        // scene stimulus and a simulator model for the vendor memories.
        avalon_write(13'h400, 32'h0000_0000); // x/y bbox packed words begin.
        for (int i = 1; i < 16; i++) begin
            avalon_write(13'h400, 32'h0000_0000);
        end

        repeat (64) @(negedge clk);
        $finish;
    end
endmodule
