module voxel_sdp_ram #(
    parameter int DATA_W = 8,
    parameter int ADDR_W = 17,
    parameter int DEPTH  = 76800
) (
    input  logic                clk,
    input  logic [ADDR_W-1:0]   rd_addr,
    output logic [DATA_W-1:0]   rd_data,
    input  logic [ADDR_W-1:0]   wr_addr,
    input  logic [DATA_W-1:0]   wr_data,
    input  logic                wr_en
);
    wire [DATA_W-1:0] q_b;

    assign rd_data = q_b;

    /*
     * Quartus stopped inferring this helper reliably once the surrounding
     * copy/scanout refactor got more complicated, and turning a 320x240
     * framebuffer into registers is fatal for fit. Use the vendor primitive
     * directly so the BRAM/Z memories stay in M10Ks.
     */
    altsyncram altsyncram_component (
        .address_a      (wr_addr),
        .address_b      (rd_addr),
        .clock0         (clk),
        .data_a         (wr_data),
        .wren_a         (wr_en),
        .q_b            (q_b),
        .aclr0          (1'b0),
        .aclr1          (1'b0),
        .addressstall_a (1'b0),
        .addressstall_b (1'b0),
        .byteena_a      (1'b1),
        .byteena_b      (1'b1),
        .clock1         (1'b1),
        .clocken0       (1'b1),
        .clocken1       (1'b1),
        .clocken2       (1'b1),
        .clocken3       (1'b1),
        .data_b         ({DATA_W{1'b0}}),
        .eccstatus      (),
        .q_a            (),
        .rden_a         (1'b1),
        .rden_b         (1'b1),
        .wren_b         (1'b0)
    );
    defparam
        altsyncram_component.address_aclr_a = "NONE",
        altsyncram_component.address_aclr_b = "NONE",
        altsyncram_component.address_reg_b = "CLOCK0",
        altsyncram_component.clock_enable_input_a = "BYPASS",
        altsyncram_component.clock_enable_input_b = "BYPASS",
        altsyncram_component.clock_enable_output_b = "BYPASS",
        altsyncram_component.indata_aclr_a = "NONE",
        altsyncram_component.intended_device_family = "Cyclone V",
        altsyncram_component.lpm_type = "altsyncram",
        altsyncram_component.numwords_a = DEPTH,
        altsyncram_component.numwords_b = DEPTH,
        altsyncram_component.operation_mode = "DUAL_PORT",
        altsyncram_component.outdata_aclr_b = "NONE",
        altsyncram_component.outdata_reg_b = "UNREGISTERED",
        altsyncram_component.power_up_uninitialized = "FALSE",
        altsyncram_component.ram_block_type = "M10K",
        altsyncram_component.read_during_write_mode_mixed_ports = "DONT_CARE",
        altsyncram_component.widthad_a = ADDR_W,
        altsyncram_component.widthad_b = ADDR_W,
        altsyncram_component.width_a = DATA_W,
        altsyncram_component.width_b = DATA_W,
        altsyncram_component.width_byteena_a = 1,
        altsyncram_component.width_byteena_b = 1;

endmodule
