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
 * each implemented by a simple-dual-port voxel_sdp_ram. Both banks are
 * exposed independently through per-bank read and write ports:
 *
 *     rd_addr_e / rd_data_e   -- even bank (linear addr LSB == 0)
 *     rd_addr_o / rd_data_o   -- odd  bank (linear addr LSB == 1)
 *     wr_addr_e / wr_data_e / wr_en_e
 *     wr_addr_o / wr_data_o / wr_en_o
 *
 * The caller passes LINEAR addresses (full ADDR_W bits) on each port;
 * the wrapper internally indexes the bank with addr[ADDR_W-1:1]. The
 * caller is responsible for steering each port to the bank whose LSB
 * matches: in particular, writes that target the even bank must arrive
 * on `wr_addr_e/wr_data_e/wr_en_e` (and likewise for odd). Mismatched
 * accesses silently land at the wrong physical entry — the LSB is
 * dropped during the bank-internal indexing.
 *
 * For 1 px/cycle paths the caller can tie both banks' addresses to the
 * same linear value, qualify each `wr_en_*` by linear `addr[0]`, and
 * pick the right `rd_data_*` using a 1-cycle-delayed LSB of the read
 * address (the underlying altsyncram has 1-cycle read latency, so the
 * delayed LSB picks whichever bank actually held the requested entry).
 *
 * For 2 px/cycle paths each lane drives its own bank's port directly,
 * with no output mux needed.
 *
 * M10K-neutral: 40,960 entries split into 2x20,480 packs into the same
 * total M10K count Quartus already produces for an unbanked instance.
 */
module voxel_banked_sdp_ram #(
    parameter int DATA_W = 16,
    parameter int ADDR_W = 16,
    parameter int DEPTH  = 40960   // total depth across both banks; must be even
) (
    input  logic                clk,
    // Per-bank read ports. Address is the LINEAR address; only bits
    // [ADDR_W-1:1] index the bank.
    input  logic [ADDR_W-1:0]   rd_addr_e,
    output logic [DATA_W-1:0]   rd_data_e,
    input  logic [ADDR_W-1:0]   rd_addr_o,
    output logic [DATA_W-1:0]   rd_data_o,
    // Per-bank write ports. Same address convention as the read ports.
    input  logic [ADDR_W-1:0]   wr_addr_e,
    input  logic [DATA_W-1:0]   wr_data_e,
    input  logic                wr_en_e,
    input  logic [ADDR_W-1:0]   wr_addr_o,
    input  logic [DATA_W-1:0]   wr_data_o,
    input  logic                wr_en_o
);
    localparam int BANK_ADDR_W = ADDR_W - 1;
    localparam int BANK_DEPTH  = DEPTH / 2;

    voxel_sdp_ram #(
        .DATA_W(DATA_W),
        .ADDR_W(BANK_ADDR_W),
        .DEPTH(BANK_DEPTH)
    ) bank_even (
        .clk     (clk),
        .rd_addr (rd_addr_e[ADDR_W-1:1]),
        .rd_data (rd_data_e),
        .wr_addr (wr_addr_e[ADDR_W-1:1]),
        .wr_data (wr_data_e),
        .wr_en   (wr_en_e)
    );

    voxel_sdp_ram #(
        .DATA_W(DATA_W),
        .ADDR_W(BANK_ADDR_W),
        .DEPTH(BANK_DEPTH)
    ) bank_odd (
        .clk     (clk),
        .rd_addr (rd_addr_o[ADDR_W-1:1]),
        .rd_data (rd_data_o),
        .wr_addr (wr_addr_o[ADDR_W-1:1]),
        .wr_data (wr_data_o),
        .wr_en   (wr_en_o)
    );
endmodule
