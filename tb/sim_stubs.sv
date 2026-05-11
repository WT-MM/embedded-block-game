// Simulation-only stubs for Intel/Quartus primitives used by voxel_gpu.
//
// These models are intentionally simple. They are good enough to let a
// source-level RTL simulator produce reset/MMIO/FSM/pipeline VCDs without
// requiring Quartus simulation libraries. They are not timing, resource, or
// vendor-accurate models of Cyclone V PLL/RAM/FIFO behavior.

`timescale 1ns/1ps

module altera_pll #(
    parameter fractional_vco_multiplier = "false",
    parameter reference_clock_frequency = "50.0 MHz",
    parameter operation_mode = "normal",
    parameter integer number_of_clocks = 1,
    parameter output_clock_frequency0 = "0 MHz",
    parameter output_clock_frequency1 = "0 MHz",
    parameter output_clock_frequency2 = "0 MHz",
    parameter output_clock_frequency3 = "0 MHz",
    parameter output_clock_frequency4 = "0 MHz",
    parameter output_clock_frequency5 = "0 MHz",
    parameter output_clock_frequency6 = "0 MHz",
    parameter output_clock_frequency7 = "0 MHz",
    parameter output_clock_frequency8 = "0 MHz",
    parameter output_clock_frequency9 = "0 MHz",
    parameter output_clock_frequency10 = "0 MHz",
    parameter output_clock_frequency11 = "0 MHz",
    parameter output_clock_frequency12 = "0 MHz",
    parameter output_clock_frequency13 = "0 MHz",
    parameter output_clock_frequency14 = "0 MHz",
    parameter output_clock_frequency15 = "0 MHz",
    parameter output_clock_frequency16 = "0 MHz",
    parameter output_clock_frequency17 = "0 MHz",
    parameter phase_shift0 = "0 ps",
    parameter phase_shift1 = "0 ps",
    parameter phase_shift2 = "0 ps",
    parameter phase_shift3 = "0 ps",
    parameter phase_shift4 = "0 ps",
    parameter phase_shift5 = "0 ps",
    parameter phase_shift6 = "0 ps",
    parameter phase_shift7 = "0 ps",
    parameter phase_shift8 = "0 ps",
    parameter phase_shift9 = "0 ps",
    parameter phase_shift10 = "0 ps",
    parameter phase_shift11 = "0 ps",
    parameter phase_shift12 = "0 ps",
    parameter phase_shift13 = "0 ps",
    parameter phase_shift14 = "0 ps",
    parameter phase_shift15 = "0 ps",
    parameter phase_shift16 = "0 ps",
    parameter phase_shift17 = "0 ps",
    parameter integer duty_cycle0 = 50,
    parameter integer duty_cycle1 = 50,
    parameter integer duty_cycle2 = 50,
    parameter integer duty_cycle3 = 50,
    parameter integer duty_cycle4 = 50,
    parameter integer duty_cycle5 = 50,
    parameter integer duty_cycle6 = 50,
    parameter integer duty_cycle7 = 50,
    parameter integer duty_cycle8 = 50,
    parameter integer duty_cycle9 = 50,
    parameter integer duty_cycle10 = 50,
    parameter integer duty_cycle11 = 50,
    parameter integer duty_cycle12 = 50,
    parameter integer duty_cycle13 = 50,
    parameter integer duty_cycle14 = 50,
    parameter integer duty_cycle15 = 50,
    parameter integer duty_cycle16 = 50,
    parameter integer duty_cycle17 = 50,
    parameter pll_type = "General",
    parameter pll_subtype = "General"
) (
    input  wire                         rst,
    input  wire                         refclk,
    input  wire                         fbclk,
    output wire [number_of_clocks-1:0]  outclk,
    output wire                         locked,
    output wire                         fboutclk
);
    assign outclk = {number_of_clocks{refclk}};
    assign locked = ~rst;
    assign fboutclk = refclk;
endmodule

module altsyncram #(
    parameter address_aclr_a = "NONE",
    parameter address_aclr_b = "NONE",
    parameter address_reg_a = "CLOCK0",
    parameter address_reg_b = "CLOCK0",
    parameter clock_enable_input_a = "BYPASS",
    parameter clock_enable_input_b = "BYPASS",
    parameter clock_enable_output_a = "BYPASS",
    parameter clock_enable_output_b = "BYPASS",
    parameter indata_aclr_a = "NONE",
    parameter init_file = "UNUSED",
    parameter intended_device_family = "Cyclone V",
    parameter lpm_hint = "",
    parameter lpm_type = "altsyncram",
    parameter integer numwords_a = 1024,
    parameter integer numwords_b = 1024,
    parameter operation_mode = "DUAL_PORT",
    parameter outdata_aclr_a = "NONE",
    parameter outdata_aclr_b = "NONE",
    parameter outdata_reg_a = "UNREGISTERED",
    parameter outdata_reg_b = "UNREGISTERED",
    parameter power_up_uninitialized = "FALSE",
    parameter ram_block_type = "M10K",
    parameter read_during_write_mode_mixed_ports = "DONT_CARE",
    parameter integer widthad_a = 10,
    parameter integer widthad_b = 10,
    parameter integer width_a = 8,
    parameter integer width_b = width_a,
    parameter integer width_byteena_a = 1,
    parameter integer width_byteena_b = 1
) (
    input  wire [widthad_a-1:0] address_a,
    input  wire [widthad_b-1:0] address_b,
    input  wire                 clock0,
    input  wire                 clock1,
    input  wire [width_a-1:0]   data_a,
    input  wire [width_b-1:0]   data_b,
    input  wire                 wren_a,
    input  wire                 wren_b,
    input  wire                 rden_a,
    input  wire                 rden_b,
    input  wire                 aclr0,
    input  wire                 aclr1,
    input  wire                 addressstall_a,
    input  wire                 addressstall_b,
    input  wire                 byteena_a,
    input  wire                 byteena_b,
    input  wire                 clocken0,
    input  wire                 clocken1,
    input  wire                 clocken2,
    input  wire                 clocken3,
    output logic [width_a-1:0]  q_a,
    output logic [width_b-1:0]  q_b,
    output wire [2:0]           eccstatus
);
    localparam int DEPTH = (numwords_a > numwords_b) ? numwords_a : numwords_b;
    localparam int WIDTH = (width_a > width_b) ? width_a : width_b;

    logic [WIDTH-1:0] mem [0:DEPTH-1];

    wire a_in_range = address_a < DEPTH[widthad_a-1:0];
    wire b_in_range = address_b < DEPTH[widthad_b-1:0];

    assign eccstatus = 3'b000;

    always_ff @(posedge clock0 or posedge aclr0) begin
        if (aclr0) begin
            q_a <= '0;
            q_b <= '0;
        end else if (clocken0) begin
            if (wren_a && byteena_a && a_in_range) begin
                mem[address_a] <= {{(WIDTH-width_a){1'b0}}, data_a};
            end
            if (rden_a && !addressstall_a && a_in_range) begin
                q_a <= mem[address_a][width_a-1:0];
            end
            if (rden_b && !addressstall_b && b_in_range) begin
                q_b <= mem[address_b][width_b-1:0];
            end
        end
    end

    always_ff @(posedge clock1 or posedge aclr1) begin
        if (aclr1) begin
            q_b <= '0;
        end else if (clocken1 && wren_b && byteena_b && b_in_range) begin
            mem[address_b] <= {{(WIDTH-width_b){1'b0}}, data_b};
        end
    end
endmodule

module dcfifo #(
    parameter intended_device_family = "Cyclone V",
    parameter lpm_hint = "",
    parameter integer lpm_numwords = 512,
    parameter lpm_showahead = "OFF",
    parameter lpm_type = "dcfifo",
    parameter integer lpm_width = 16,
    parameter integer lpm_widthu = 9,
    parameter overflow_checking = "ON",
    parameter integer rdsync_delaypipe = 4,
    parameter read_aclr_synch = "OFF",
    parameter underflow_checking = "ON",
    parameter use_eab = "ON",
    parameter write_aclr_synch = "OFF",
    parameter integer wrsync_delaypipe = 4
) (
    input  wire                    aclr,
    input  wire [lpm_width-1:0]    data,
    input  wire                    rdclk,
    input  wire                    rdreq,
    input  wire                    wrclk,
    input  wire                    wrreq,
    output logic [lpm_width-1:0]   q,
    output logic                   rdempty,
    output logic                   rdfull,
    output logic                   wrempty,
    output logic                   wrfull,
    output logic [lpm_widthu-1:0]  rdusedw,
    output logic [lpm_widthu-1:0]  wrusedw,
    output wire [2:0]              eccstatus
);
    localparam int DEPTH = lpm_numwords;
    logic [lpm_width-1:0] mem [0:DEPTH-1];
    logic [lpm_widthu-1:0] wr_ptr;
    logic [lpm_widthu-1:0] rd_ptr;
    logic [lpm_widthu:0] count;

    assign eccstatus = 3'b000;

    always_comb begin
        rdempty = (count == 0);
        wrempty = rdempty;
        rdfull = (count >= DEPTH[lpm_widthu:0]);
        wrfull = rdfull;
        rdusedw = count[lpm_widthu-1:0];
        wrusedw = count[lpm_widthu-1:0];
    end

    always_ff @(posedge wrclk or posedge aclr) begin
        if (aclr) begin
            wr_ptr <= '0;
            count <= '0;
        end else if (wrreq && !wrfull) begin
            mem[wr_ptr] <= data;
            wr_ptr <= wr_ptr + 1'b1;
            count <= count + 1'b1;
        end
    end

    always_ff @(posedge rdclk or posedge aclr) begin
        if (aclr) begin
            rd_ptr <= '0;
            q <= '0;
        end else if (rdreq && !rdempty) begin
            q <= mem[rd_ptr];
            rd_ptr <= rd_ptr + 1'b1;
            count <= count - 1'b1;
        end
    end
endmodule
