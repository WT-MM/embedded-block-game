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
// Two-read-port texture (May 2026): the 2 px/cycle rasterizer needs to
// look up two texels per cycle (even-x and odd-x lanes). Cyclone V
// altsyncram in BIDIR_DUAL_PORT mode is protected/restrictive (it
// requires clock1 to be driven and rejects a tied-off second port that
// re-uses clock0), so the canonical pattern for a "true dual read ROM"
// here is to instantiate two single-port (operation_mode = "ROM")
// altsyncrams that share the same `.mif` init file. Each instance
// is a normal 1-cycle ROM; together they serve two independent reads
// per cycle. M10K cost is 2x the unbanked atlas, so textured descriptors
// reserve tex_or_color[6:0] for the 128-tile index and keep
// tex_or_color[7] as the repeat-UV flag in the caller.
//
// The atlas is initialised from a `.mif` file (see hw/voxel_gpu/scripts/generate_textures.py)
// because altsyncram's init_file does not accept Verilog $readmemh-style
// plain-byte dumps. We share that same `.mif` with the Python virtual
// hardware, so simulation and synthesis read from a single source.
// ====================================================================
module voxel_texture_rom #(
    parameter int DATA_W = 8,
    parameter int ADDR_W = 15,
    parameter int DEPTH  = 32768,
    parameter      INIT_FILE = "voxel_gpu/assets/textures.mif"
) (
    input  logic                clk,
    // Port A: the existing single-pixel texture read path.
    input  logic [ADDR_W-1:0]   rd_addr,
    output logic [DATA_W-1:0]   rd_data,
    // Port B: second independent read port for the 2 px/cycle
    // rasterizer's odd-x lane. Tie rd_addr_b to {ADDR_W{1'b0}} when
    // unused; the M10K cost is paid regardless because the second ROM
    // copy is always synthesized.
    input  logic [ADDR_W-1:0]   rd_addr_b,
    output logic [DATA_W-1:0]   rd_data_b
);
    wire [DATA_W-1:0] q_a;
    wire [DATA_W-1:0] q_b;
    assign rd_data   = q_a;
    assign rd_data_b = q_b;

    // Port A: single-port ROM, identical to the original instance.
    altsyncram altsyncram_rom_a (
        .address_a      (rd_addr),
        .clock0         (clk),
        .q_a            (q_a),
        .aclr0          (1'b0),
        .aclr1          (1'b0),
        .address_b      ({ADDR_W{1'b1}}),
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
        .q_b            (),
        .rden_a         (1'b1),
        .rden_b         (1'b1),
        .wren_a         (1'b0),
        .wren_b         (1'b0)
    );
    defparam
        altsyncram_rom_a.address_aclr_a = "NONE",
        altsyncram_rom_a.clock_enable_input_a = "BYPASS",
        altsyncram_rom_a.clock_enable_output_a = "BYPASS",
        altsyncram_rom_a.init_file = INIT_FILE,
        altsyncram_rom_a.intended_device_family = "Cyclone V",
        altsyncram_rom_a.lpm_hint = "ENABLE_RUNTIME_MOD=NO",
        altsyncram_rom_a.lpm_type = "altsyncram",
        altsyncram_rom_a.numwords_a = DEPTH,
        altsyncram_rom_a.operation_mode = "ROM",
        altsyncram_rom_a.outdata_aclr_a = "NONE",
        altsyncram_rom_a.outdata_reg_a = "UNREGISTERED",
        altsyncram_rom_a.ram_block_type = "M10K",
        altsyncram_rom_a.widthad_a = ADDR_W,
        altsyncram_rom_a.width_a = DATA_W,
        altsyncram_rom_a.width_byteena_a = 1;

    // Port B: a second, independent copy of the same atlas. Same `.mif`
    // -> identical contents. This is the canonical Cyclone V pattern
    // for a "true dual read ROM" when BIDIR_DUAL_PORT cannot be used.
    altsyncram altsyncram_rom_b (
        .address_a      (rd_addr_b),
        .clock0         (clk),
        .q_a            (q_b),
        .aclr0          (1'b0),
        .aclr1          (1'b0),
        .address_b      ({ADDR_W{1'b1}}),
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
        .q_b            (),
        .rden_a         (1'b1),
        .rden_b         (1'b1),
        .wren_a         (1'b0),
        .wren_b         (1'b0)
    );
    defparam
        altsyncram_rom_b.address_aclr_a = "NONE",
        altsyncram_rom_b.clock_enable_input_a = "BYPASS",
        altsyncram_rom_b.clock_enable_output_a = "BYPASS",
        altsyncram_rom_b.init_file = INIT_FILE,
        altsyncram_rom_b.intended_device_family = "Cyclone V",
        altsyncram_rom_b.lpm_hint = "ENABLE_RUNTIME_MOD=NO",
        altsyncram_rom_b.lpm_type = "altsyncram",
        altsyncram_rom_b.numwords_a = DEPTH,
        altsyncram_rom_b.operation_mode = "ROM",
        altsyncram_rom_b.outdata_aclr_a = "NONE",
        altsyncram_rom_b.outdata_reg_a = "UNREGISTERED",
        altsyncram_rom_b.ram_block_type = "M10K",
        altsyncram_rom_b.widthad_a = ADDR_W,
        altsyncram_rom_b.width_a = DATA_W,
        altsyncram_rom_b.width_byteena_a = 1;

endmodule
