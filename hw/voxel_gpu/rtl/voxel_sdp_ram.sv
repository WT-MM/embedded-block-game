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

/*
 * voxel_banked_sdp_ram
 * --------------------
 * Splits the array into an even-x bank and an odd-x bank using addr[0],
 * each implemented by a simple-dual-port voxel_sdp_ram.
 *
 * For step 2 of the 2 px/cycle project, the wrapper exposes the original
 * 1R/1W external interface (rd_addr/rd_data/wr_addr/wr_data/wr_en).
 * Internally it routes the write to the matching bank via wr_addr[0] and
 * picks the read result via a 1-cycle-delayed rd_addr[0] mux that lines
 * up with the underlying altsyncram's 1-cycle read latency. Functionally
 * this is identical to a single SDP RAM of the original depth.
 *
 * For step 4 (lane duplication), the wrapper will be extended to also
 * expose independent per-bank ports so the rasterizer can issue an
 * even-x and an odd-x read+write in the same cycle without going through
 * the unified mux. The two SDP backing RAMs are already separate
 * hardware -- step 4 is purely an API change at this wrapper.
 *
 * M10K-neutral: 40,960 entries split into 2x20,480 packs into the same
 * total M10K count Quartus already produces for the unbanked instance.
 */
module voxel_banked_sdp_ram #(
    parameter int DATA_W = 16,
    parameter int ADDR_W = 16,
    parameter int DEPTH  = 40960   // total depth across both banks; must be even
) (
    input  logic                clk,
    input  logic [ADDR_W-1:0]   rd_addr,
    output logic [DATA_W-1:0]   rd_data,
    input  logic [ADDR_W-1:0]   wr_addr,
    input  logic [DATA_W-1:0]   wr_data,
    input  logic                wr_en
);
    localparam int BANK_ADDR_W = ADDR_W - 1;
    localparam int BANK_DEPTH  = DEPTH / 2;

    logic [DATA_W-1:0] rd_data_even, rd_data_odd;
    logic              rd_sel_q;

    always_ff @(posedge clk) rd_sel_q <= rd_addr[0];

    assign rd_data = rd_sel_q ? rd_data_odd : rd_data_even;

    voxel_sdp_ram #(
        .DATA_W(DATA_W),
        .ADDR_W(BANK_ADDR_W),
        .DEPTH(BANK_DEPTH)
    ) bank_even (
        .clk     (clk),
        .rd_addr (rd_addr[ADDR_W-1:1]),
        .rd_data (rd_data_even),
        .wr_addr (wr_addr[ADDR_W-1:1]),
        .wr_data (wr_data),
        .wr_en   (wr_en && (wr_addr[0] == 1'b0))
    );

    voxel_sdp_ram #(
        .DATA_W(DATA_W),
        .ADDR_W(BANK_ADDR_W),
        .DEPTH(BANK_DEPTH)
    ) bank_odd (
        .clk     (clk),
        .rd_addr (rd_addr[ADDR_W-1:1]),
        .rd_data (rd_data_odd),
        .wr_addr (wr_addr[ADDR_W-1:1]),
        .wr_data (wr_data),
        .wr_en   (wr_en && (wr_addr[0] == 1'b1))
    );
endmodule
