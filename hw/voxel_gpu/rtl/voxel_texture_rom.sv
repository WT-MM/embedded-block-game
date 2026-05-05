// Texture-atlas ROM. Previously we let Quartus infer this from
//
//     (* ramstyle = "M10K" *) logic [7:0] texture_mem [0:TEXTURE_BYTES-1];
//     ...
//     always_ff @(posedge clk) tex_rd_data <= texture_mem[pipe2_tex_addr];
//
// but Quartus is free to implement that pattern with EITHER a 1-cycle
// latency (addr_reg_a = CLOCK0, outdata_reg_a = UNREGISTERED, tex_rd_data
// is absorbed as the output register) OR a 2-cycle latency (addr_reg_a +
// outdata_reg_a BOTH registered, with tex_rd_data acting as a post-RAM
// flop on top). On real silicon we saw the latter: tex_rd_data arrived
// one cycle later than the rest of the draw_pipe metadata, so the first
// pixel of each quad read the previous quad's final texel. That showed
// up as 1-pixel colored fringes on every block edge whose color depended
// on the immediately preceding quad's texture (e.g. stones next to sky
// tiles picked up light-blue fringes) -- a "chromatic aberration" / edge
// flicker symptom that did not go away even after pipelining the palette
// read explicitly.
//
// Instantiating altsyncram directly with address_reg_a = CLOCK0 and
// outdata_reg_a = UNREGISTERED pins the latency to a known 1 cycle, so
// pipe2_tex_addr at cycle T drives rd_data at cycle T+1, in lockstep
// with pipe2 -> draw_pipe.
//
// True dual-port (May 2026): the ROM now exposes a second independent
// read port (rd_addr_b / rd_data_b) so the 2 px/cycle rasterizer can
// look up two texels per cycle without serialising. Implementation
// changed from operation_mode = "ROM" (port A only) to
// "BIDIR_DUAL_PORT" with both wren tied off plus the same init_file,
// which Quartus implements as a true dual-read M10K. Port-B inputs
// can be tied off if a caller only needs one read; the unused port
// then never fires reads but the M10K cost is paid regardless.
//
// The atlas is initialised from a `.mif` file (see hw/voxel_gpu/scripts/generate_textures.py)
// because altsyncram's init_file does not accept Verilog $readmemh-style
// plain-byte dumps. We share that same `.mif` with the Python virtual
// hardware, so simulation and synthesis read from a single source.
// ====================================================================
module voxel_texture_rom #(
    parameter int DATA_W = 8,
    parameter int ADDR_W = 14,
    parameter int DEPTH  = 16384,
    parameter      INIT_FILE = "voxel_gpu/assets/textures.mif"
) (
    input  logic                clk,
    // Port A: the existing single-pixel texture read path.
    input  logic [ADDR_W-1:0]   rd_addr,
    output logic [DATA_W-1:0]   rd_data,
    // Port B: second independent read port for the 2 px/cycle
    // rasterizer's odd-x lane. Tie rd_addr_b to {ADDR_W{1'b0}} when
    // unused; the M10K cost is the same either way.
    input  logic [ADDR_W-1:0]   rd_addr_b,
    output logic [DATA_W-1:0]   rd_data_b
);
    wire [DATA_W-1:0] q_a;
    wire [DATA_W-1:0] q_b;
    assign rd_data   = q_a;
    assign rd_data_b = q_b;

    altsyncram altsyncram_rom (
        .address_a      (rd_addr),
        .address_b      (rd_addr_b),
        .clock0         (clk),
        .q_a            (q_a),
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
        .data_a         ({DATA_W{1'b0}}),
        .data_b         ({DATA_W{1'b0}}),
        .eccstatus      (),
        .rden_a         (1'b1),
        .rden_b         (1'b1),
        .wren_a         (1'b0),
        .wren_b         (1'b0)
    );
    /*
     * BIDIR_DUAL_PORT with both wren tied off and an init_file is the
     * altsyncram pattern for a true 2-read-port ROM on Cyclone V. Both
     * ports get registered addresses (CLOCK0) and unregistered outputs,
     * matching the original 1-cycle latency exactly. clock1 is unused
     * because both ports run on clock0.
     */
    defparam
        altsyncram_rom.address_aclr_a = "NONE",
        altsyncram_rom.address_aclr_b = "NONE",
        altsyncram_rom.address_reg_b  = "CLOCK0",
        altsyncram_rom.clock_enable_input_a = "BYPASS",
        altsyncram_rom.clock_enable_input_b = "BYPASS",
        altsyncram_rom.clock_enable_output_a = "BYPASS",
        altsyncram_rom.clock_enable_output_b = "BYPASS",
        altsyncram_rom.init_file = INIT_FILE,
        altsyncram_rom.intended_device_family = "Cyclone V",
        altsyncram_rom.lpm_hint = "ENABLE_RUNTIME_MOD=NO",
        altsyncram_rom.lpm_type = "altsyncram",
        altsyncram_rom.numwords_a = DEPTH,
        altsyncram_rom.numwords_b = DEPTH,
        altsyncram_rom.operation_mode = "BIDIR_DUAL_PORT",
        altsyncram_rom.outdata_aclr_a = "NONE",
        altsyncram_rom.outdata_aclr_b = "NONE",
        altsyncram_rom.outdata_reg_a = "UNREGISTERED",
        altsyncram_rom.outdata_reg_b = "UNREGISTERED",
        altsyncram_rom.power_up_uninitialized = "FALSE",
        altsyncram_rom.ram_block_type = "M10K",
        altsyncram_rom.read_during_write_mode_port_a = "NEW_DATA_NO_NBE_READ",
        altsyncram_rom.read_during_write_mode_port_b = "NEW_DATA_NO_NBE_READ",
        altsyncram_rom.widthad_a = ADDR_W,
        altsyncram_rom.widthad_b = ADDR_W,
        altsyncram_rom.width_a = DATA_W,
        altsyncram_rom.width_b = DATA_W,
        altsyncram_rom.width_byteena_a = 1,
        altsyncram_rom.width_byteena_b = 1;

endmodule
