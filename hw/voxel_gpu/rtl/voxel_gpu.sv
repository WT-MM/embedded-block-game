// voxel_gpu.sv — SDRAM-backed display path with RGB565 external frames.
//
// The rasterizer renders at true 640x480 through an explicit 640x64 BRAM band
// cache. Userspace bins descriptors into eight vertical passes and brackets each
// pass with BEGIN_BAND / END_BAND CSRs. Palette entries are still the
// source-color ABI, but resolved pixels are stored as RGB565 so translucent
// quads can alpha blend against the existing destination pixel.
// FLIP no longer copies a full BRAM framebuffer. Instead:
//   * BEGIN_BAND clears the resident color/z band,
//   * the rasterizer writes only pixels that fall inside that resident band,
//   * END_BAND flushes the color band to the inactive SDRAM color frame, and
//   * scanout reads the active SDRAM frame through small line buffers.
//
// This keeps BRAM usage close to the indexed design while making the external
// framebuffer itself true color.

module voxel_gpu (
    input  logic        clk,
    input  logic        reset,

    // Avalon-MM slave (address units = WORDS; writedata is 32 bits).
    input  logic [12:0] address,
    input  logic        chipselect,
    input  logic        write,
    input  logic [31:0] writedata,
    input  logic  [3:0] byteenable,
    output logic [31:0] readdata,

    // VGA conduit
    output logic  [7:0] VGA_R,
    output logic  [7:0] VGA_G,
    output logic  [7:0] VGA_B,
    output logic        VGA_CLK,
    output logic        VGA_HS,
    output logic        VGA_VS,
    output logic        VGA_BLANK_n,
    output logic        VGA_SYNC_n,

    // Board SDR SDRAM conduit
    output logic [12:0] DRAM_ADDR,
    output logic  [1:0] DRAM_BA,
    output logic        DRAM_CAS_N,
    output logic        DRAM_CKE,
    output logic        DRAM_CLK,
    output logic        DRAM_CS_N,
    inout  wire [15:0]  DRAM_DQ,
    output logic        DRAM_LDQM,
    output logic        DRAM_RAS_N,
    output logic        DRAM_UDQM,
    output logic        DRAM_WE_N
);

    localparam logic [12:0] ADDR_CONTROL  = 13'h000;
    localparam logic [12:0] ADDR_STATUS   = 13'h001;
    localparam logic [12:0] ADDR_FRAMECNT = 13'h002;
    localparam logic [12:0] ADDR_PAL_ADDR = 13'h003;
    localparam logic [12:0] ADDR_PAL_DATA = 13'h004;
    localparam logic [12:0] ADDR_FOG_RANGE = 13'h005;
    localparam logic [12:0] ADDR_FOG_CTRL = 13'h006;
    localparam logic [12:0] ADDR_EXTMEM_CTRL = 13'h007;
    localparam logic [12:0] ADDR_EXTMEM_FRONT = 13'h008;
    localparam logic [12:0] ADDR_EXTMEM_BACK = 13'h009;
    localparam logic [12:0] ADDR_EXTMEM_STRIDE = 13'h00A;
    localparam logic [12:0] ADDR_EXTMEM_TILE = 13'h00B;
    localparam logic [12:0] ADDR_EXTMEM_STAT = 13'h00C;
    localparam logic [12:0] ADDR_BAND_INDEX = 13'h00D;
    localparam logic [12:0] ADDR_BAND_CTRL = 13'h00E;
    localparam logic [12:0] ADDR_PERF_DRAW_ACT  = 13'h010;
    localparam logic [12:0] ADDR_PERF_DRAW_IDLE = 13'h011;
    localparam logic [12:0] ADDR_PERF_FLUSH_ACT = 13'h012;
    localparam logic [12:0] ADDR_PERF_FLUSH_STL = 13'h013;
    localparam logic [12:0] ADDR_PERF_INIT      = 13'h014;
    localparam logic [12:0] ADDR_PERF_LOAD      = 13'h015;
    localparam logic [12:0] ADDR_FIFO_LO  = 13'h400;  // 0x1000
    localparam logic [12:0] ADDR_FIFO_HI  = 13'hC00;  // 0x3000 (exclusive)

    localparam int FB_WIDTH       = 640;
    localparam int FB_HEIGHT      = 480;
    localparam int FB_PIXELS      = FB_WIDTH * FB_HEIGHT;
    localparam int BAND_HEIGHT    = 64;
    localparam int BAND_PIXELS    = FB_WIDTH * BAND_HEIGHT;
    localparam int BAND_COUNT     = 8;
    localparam int FIFO_DEPTH     = 2048;
    localparam int BASE_QUAD_WORDS = 16;
    localparam int UV_QUAD_WORDS   = 16;
    localparam int MAX_DESC_WORDS  = BASE_QUAD_WORDS + UV_QUAD_WORDS;
    localparam int TEXTURE_BYTES   = 64 * 16 * 16;
    localparam int LINE_WORDS      = FB_WIDTH;
    localparam int COPY_BURST_WORDS = 64;
    localparam int READ_BURST_WORDS = 64;
    /*
     * Tier-1 SDRAM-arbitration plan: bg flush stages words into the WR FIFO
     * during scanout-read windows, then drains them via WR_LENGTH bursts the
     * moment scanout_write_slack opens. The Sdram_WR_FIFO IP is 512 deep
     * (sdram_local_test/Sdram_WR_FIFO.v: lpm_numwords = 512); 224 leaves
     * 288 words of headroom for in-flight pushes after !sdram_wr_full
     * deasserts. flush_active stays high until sdram_wr_use==0 + post-empty
     * settling (COPY_DRAIN_CYCLES), so the back-buffer flip cannot race
     * with words still queued in the FIFO regardless of the high-water.
     */
    localparam logic [8:0] COPY_WR_FIFO_HIGH_WATER = 9'd224;
    localparam logic [7:0] COPY_DRAIN_CYCLES = 8'd128;
    localparam int SDRAM_ADDR_W    = 25;
    /* Flush cycles required to drain the rasterizer pipeline before
     * leaving ST_DRAW. Must match (number of *_valid flops between
     * ST_FETCH and commit_color). Raising to 14 covers both palette
     * pipeline stages (pal_rd + plr) that live between draw_pipe and
     * fog0. The pal_rd stage registers the palette address and the
     * plr stage registers the palette output, which makes the timing
     * deterministic regardless of whether Quartus implements the
     * palette array as MLAB (async read) or M10K (sync read). */
    localparam logic [3:0] DRAW_FLUSH_CYCLES = 4'd14;
    localparam logic [24:0] FB_WORDS_25 = 25'd307200;
    localparam logic [24:0] READ_BURST_WORDS_25 = 25'd64;
    localparam logic [9:0]  LINE_WORDS_10 = 10'd640;
    localparam logic [8:0]  COPY_BURST_WORDS_9 = 9'd64;
    localparam logic [8:0]  READ_BURST_WORDS_9 = 9'd64;
    localparam logic [10:0] ACTIVE_WRITE_END_HCOUNT = 11'd960;
    localparam int EXTMEM_SKY_GRADIENT_CLEAR_BIT = 5;
    localparam logic [7:0] PAL_SKY_GRADIENT_BASE = 8'd40;
    localparam logic [7:0] PAL_SKY_GRADIENT_LAST = 8'd63;
    localparam logic [31:0] DEFAULT_EXTMEM_CTRL = 32'h0000_002B;
    localparam logic [31:0] DEFAULT_EXTMEM_FRONT_BASE = 32'd0;
    localparam logic [31:0] DEFAULT_EXTMEM_BACK_BASE = 32'd1048576; // 1MB
    localparam logic [31:0] DEFAULT_EXTMEM_STRIDE = 32'd1280;
    localparam logic [31:0] DEFAULT_EXTMEM_Z_BASE = 32'd2097152; // 2MB
    localparam int SDRAM_POWERUP_HOLD_CYCLES = 200000;
    localparam int SDRAM_INIT_WAIT_CYCLES = 32000;
    localparam logic [17:0] SDRAM_POWERUP_HOLD_LAST = 18'd199999;
    localparam logic [15:0] SDRAM_INIT_WAIT_LAST = 16'd31999;
    localparam int FLAG_TEX_BIT        = 0;
    localparam int FLAG_ZTEST_BIT      = 1;
    localparam int FLAG_ALPHA_KEY_BIT  = 2;
    localparam int FLAG_FOG_BIT        = 3;
    localparam int FLAG_LIGHT_LSB      = 4;
    localparam int FLAG_LIGHT_MSB      = 5;
    localparam int FLAG_ALPHA_LSB      = 6;
    localparam int FLAG_ALPHA_MSB      = 7;

    typedef enum logic [3:0] {
        ST_IDLE              = 4'd0,
        ST_CLEAR             = 4'd1,
        ST_FETCH             = 4'd2,
        ST_SETUP             = 4'd3,
        ST_DRAW              = 4'd4,
        ST_DRAW_FLUSH        = 4'd5,
        ST_CACHE_EVICT       = 4'd6,
        ST_CACHE_FLUSH_COLOR = 4'd7,
        ST_CACHE_FLUSH_Z     = 4'd8,
        ST_CACHE_SELECT_FILL = 4'd9,
        ST_CACHE_INIT        = 4'd10,
        ST_CACHE_LOAD_COLOR  = 4'd11,
        ST_CACHE_START_LOAD_Z = 4'd12,
        ST_CACHE_LOAD_Z      = 4'd13,
        ST_CACHE_DRAIN_COLOR = 4'd14,
        ST_CACHE_DRAIN_Z     = 4'd15
    } engine_state_t;

    engine_state_t state;

    logic        ctrl_en;
    logic        ctrl_ien;
    logic        ctrl_flp_pending;
    logic        clear_pending;
    logic [31:0] frame_count;
    logic  [7:0] pal_addr;
    logic        fog_enable;
    logic  [7:0] fog_color;
    logic [15:0] fog_start_dist;
    logic [15:0] fog_end_dist;
    logic [15:0] fog_inv_proj_sq;
    logic [31:0] extmem_ctrl;
    logic [31:0] extmem_front_base;
    logic [31:0] extmem_back_base;
    logic [31:0] extmem_stride_bytes;
    logic [31:0] extmem_tile_cfg;
    logic [31:0] extmem_dma_status;
    logic        vsy_latch;
    logic        display_sel;             // 0 => extmem_front_base is visible
    logic        display_valid;
    logic        copy_target_sel;         // inactive SDRAM color frame rendered this frame
    logic        copy_complete_pending;   // completed render frame waits for vsync display swap

    // Per-frame perf counters (free-running, reset on FLIP write).
    // Software reads via ADDR_PERF_* before issuing the FLIP that ends
    // the frame, so the values reflect the just-completed frame.
    // 50 MHz × 50000 cycles = 1 ms.
    logic [31:0] perf_draw_active;   // ST_DRAW/ST_DRAW_FLUSH committing pixel
    logic [31:0] perf_draw_idle;     // ST_DRAW/ST_DRAW_FLUSH no commit (starved)
    logic [31:0] perf_flush_active;  // bg flush running AND word pushed this cyc
    logic [31:0] perf_flush_stall;   // bg flush running AND no push (SDRAM stall)
    logic [31:0] perf_init;          // ST_CACHE_INIT
    logic [31:0] perf_load;          // ST_CACHE_LOAD_*/ST_CACHE_DRAIN_*

    // Palette lookup path: draw_pipe_color -> apply_light_bank ->
    // pal_rd_src_addr (registered) -> palette[] -> plr_src_rgb
    // (registered) -> rgb888_to_rgb565 -> fog0_src_rgb565 (registered).
    //
    // The pal_rd + plr split flops the palette address and its read
    // output in two separate cycles, so the pipeline stays correct
    // whether Quartus implements this array as MLAB (async read) or
    // demotes it to M10K (sync read). The MLAB attribute still keeps
    // the read cheap on paper; the explicit two-stage pipeline is the
    // correctness guarantee. See PROJECT_NOTES.md for the history.
    (* ramstyle = "MLAB" *) logic [23:0] palette [0:255];
    (* ramstyle = "M10K" *) logic [31:0] fifo_mem [0:FIFO_DEPTH-1];
    // texture_mem is implemented as an explicit altsyncram ROM below
    // (see voxel_texture_rom). Do NOT reintroduce an inferred array here:
    // inference lets Quartus silently pick between 1-cycle and 2-cycle
    // read latency, which is what caused the quad-boundary colored
    // fringes / "chromatic aberration" we chased for weeks.
    (* ramstyle = "MLAB" *) logic [31:0] recip_lut [0:1024];

    logic [10:0] fifo_wr_ptr;
    logic [10:0] fifo_rd_ptr;
    logic [11:0] fifo_count;
    wire         fifo_full  = (fifo_count == 12'd2048);
    wire         fifo_empty = (fifo_count == 0);
    wire [31:0]  fifo_head  = fifo_mem[fifo_rd_ptr];

    logic [31:0] desc_words [0:MAX_DESC_WORDS-1];
    logic  [5:0] fetch_count;
    logic  [3:0] draw_flush_count;

    logic [15:0] clear_addr;
    logic [15:0] draw_row_base;
    logic  [9:0] draw_x_min, draw_x_max, draw_x_cur;
    logic  [8:0] draw_y_min;
    logic  [8:0] draw_y_max, draw_y_cur;
    logic        draw_row_inside;  // set when draw_inside seen on current row
    logic  [7:0] draw_tex_or_color;
    logic  [7:0] draw_flags;
    logic [15:0] draw_z0;
    logic signed [15:0] draw_dz_dx;
    logic signed [15:0] draw_dz_dy;
    logic signed [31:0] draw_uw_0;
    logic signed [31:0] draw_uw_dx;
    logic signed [31:0] draw_uw_dy;
    logic signed [31:0] draw_vw_0;
    logic signed [31:0] draw_vw_dx;
    logic signed [31:0] draw_vw_dy;
    logic signed [31:0] draw_iw_0;
    logic signed [31:0] draw_iw_dx;
    logic signed [31:0] draw_iw_dy;
    logic signed [31:0] edge_A [0:3];
    logic signed [31:0] edge_B [0:3];
    logic signed [31:0] edge_C [0:3];
    logic signed [63:0] edge_row_val [0:3];
    logic signed [63:0] edge_cur_val [0:3];
    logic signed [47:0] z_row_val;
    logic signed [47:0] z_cur_val;
    logic signed [63:0] uw_row_val, uw_cur_val;
    logic signed [63:0] vw_row_val, vw_cur_val;
    logic signed [63:0] iw_row_val, iw_cur_val;
    logic        pipe0_valid;
    logic        pipe0_inside;
    logic        pipe0_ztest;
    logic        pipe0_textured;
    logic        pipe0_alpha_key;
    logic  [1:0] pipe0_alpha;
    logic        pipe0_fog;
    logic  [1:0] pipe0_light_bank;
    logic  [7:0] pipe0_tex_or_color;
    logic [15:0] pipe0_addr;
    logic [15:0] pipe0_z;
    logic  [9:0] pipe0_x;
    logic  [8:0] pipe0_y;
    logic signed [31:0] pipe0_uw_q;
    logic signed [31:0] pipe0_vw_q;
    logic [31:0] pipe0_iw_q;
    logic        recip0_valid;
    logic        recip0_inside;
    logic        recip0_ztest;
    logic        recip0_textured;
    logic        recip0_alpha_key;
    logic  [1:0] recip0_alpha;
    logic        recip0_fog;
    logic  [1:0] recip0_light_bank;
    logic  [7:0] recip0_tex_or_color;
    logic [15:0] recip0_addr;
    logic [15:0] recip0_z;
    logic  [9:0] recip0_x;
    logic  [8:0] recip0_y;
    logic signed [31:0] recip0_uw_q;
    logic signed [31:0] recip0_vw_q;
    logic        recip0_iw_zero;
    logic  [5:0] recip0_iw_msb;
    logic [31:0] recip0_iw_norm_q;
    logic        recip1_valid;
    logic        recip1_inside;
    logic        recip1_ztest;
    logic        recip1_textured;
    logic        recip1_alpha_key;
    logic  [1:0] recip1_alpha;
    logic        recip1_fog;
    logic  [1:0] recip1_light_bank;
    logic  [7:0] recip1_tex_or_color;
    logic [15:0] recip1_addr;
    logic [15:0] recip1_z;
    logic [15:0] recip1_z_ref;
    logic [15:0] recip1_dst_rgb565;
    logic  [9:0] recip1_x;
    logic  [8:0] recip1_y;
    logic signed [31:0] recip1_uw_q;
    logic signed [31:0] recip1_vw_q;
    logic        recip1_iw_zero;
    logic  [5:0] recip1_iw_msb;
    logic  [5:0] recip1_iw_lut_frac;
    logic [31:0] recip1_w_norm_lo;
    logic [31:0] recip1_w_norm_hi;
    logic        recip2_valid;
    logic        recip2_inside;
    logic        recip2_ztest;
    logic        recip2_textured;
    logic        recip2_alpha_key;
    logic  [1:0] recip2_alpha;
    logic        recip2_fog;
    logic  [1:0] recip2_light_bank;
    logic  [7:0] recip2_tex_or_color;
    logic [15:0] recip2_addr;
    logic [15:0] recip2_z;
    logic [15:0] recip2_z_ref;
    logic [15:0] recip2_dst_rgb565;
    logic  [9:0] recip2_x;
    logic  [8:0] recip2_y;
    logic signed [31:0] recip2_uw_q;
    logic signed [31:0] recip2_vw_q;
    logic        recip2_iw_zero;
    logic  [5:0] recip2_iw_msb;
    logic [31:0] recip2_w_norm_q;
    logic        pipe1_valid;
    logic        pipe1_inside;
    logic        pipe1_ztest;
    logic        pipe1_textured;
    logic        pipe1_alpha_key;
    logic  [1:0] pipe1_alpha;
    logic        pipe1_fog;
    logic  [1:0] pipe1_light_bank;
    logic  [7:0] pipe1_tex_or_color;
    logic [15:0] pipe1_addr;
    logic [15:0] pipe1_z;
    logic [15:0] pipe1_z_ref;
    logic [15:0] pipe1_dst_rgb565;
    logic  [9:0] pipe1_x;
    logic  [8:0] pipe1_y;
    logic signed [31:0] pipe1_uw_q;
    logic signed [31:0] pipe1_vw_q;
    logic [31:0] pipe1_w_q;
    logic        tex0_valid;
    logic        tex0_inside;
    logic        tex0_ztest;
    logic        tex0_textured;
    logic        tex0_alpha_key;
    logic  [1:0] tex0_alpha;
    logic        tex0_fog;
    logic  [1:0] tex0_light_bank;
    logic  [7:0] tex0_tex_or_color;
    logic [15:0] tex0_addr;
    logic [15:0] tex0_z;
    logic [15:0] tex0_z_ref;
    logic [15:0] tex0_dst_rgb565;
    logic  [9:0] tex0_x;
    logic  [8:0] tex0_y;
    logic [31:0] tex0_w_q;
    logic signed [63:0] tex0_u_prod;
    logic signed [63:0] tex0_v_prod;
    logic        pipe2_valid;
    logic        pipe2_inside;
    logic        pipe2_ztest;
    logic        pipe2_textured;
    logic        pipe2_alpha_key;
    logic  [1:0] pipe2_alpha;
    logic        pipe2_fog;
    logic  [1:0] pipe2_light_bank;
    logic  [7:0] pipe2_tex_or_color;
    logic [15:0] pipe2_addr;
    logic [15:0] pipe2_z;
    logic [15:0] pipe2_z_ref;
    logic [15:0] pipe2_dst_rgb565;
    logic  [9:0] pipe2_x;
    logic  [8:0] pipe2_y;
    logic [31:0] pipe2_w_q;
    logic [13:0] pipe2_tex_addr;
    logic        draw_pipe_valid;
    logic        draw_pipe_inside;
    logic        draw_pipe_ztest;
    logic        draw_pipe_textured;
    logic        draw_pipe_alpha_key;
    logic  [1:0] draw_pipe_alpha;
    logic        draw_pipe_fog;
    logic  [1:0] draw_pipe_light_bank;
    logic  [7:0] draw_pipe_tex_or_color;
    logic [15:0] draw_pipe_addr;
    logic [15:0] draw_pipe_z;
    logic [15:0] draw_pipe_z_ref;
    logic [15:0] draw_pipe_dst_rgb565;
    logic  [9:0] draw_pipe_x;
    logic  [8:0] draw_pipe_y;
    logic [31:0] draw_pipe_w_q;
    logic        draw_is_band_primer;

    /* Palette-address register (pal_rd) stage: first half of the
     * pipelined palette read. We register the palette source/fog
     * addresses coming out of apply_light_bank here so the palette
     * array is indexed by a stable, flopped address on the following
     * cycle. The plr stage below then captures the palette output.
     * Splitting the read into two cycles (address flop, then data
     * flop) makes the timing deterministic whether Quartus implements
     * the palette array as MLAB (async read) or silently demotes it
     * to M10K (sync read with a registered address internally). The
     * old single-plr layout only worked when MLAB was inferred; if
     * Quartus added its own address register on top we ended up one
     * cycle behind and bled the previous quad's palette entry into
     * the first pixel of each new quad, which manifested as colored
     * fringes on every block edge (worst on stone, where any non-gray
     * leak is maximally visible). */
    logic        pal_rd_valid;
    logic        pal_rd_pass;
    logic        pal_rd_ztest;
    logic  [1:0] pal_rd_alpha;
    logic        pal_rd_fog;
    logic [15:0] pal_rd_addr;
    logic [15:0] pal_rd_z;
    logic  [7:0] pal_rd_src_addr;
    logic  [7:0] pal_rd_fog_addr;
    logic [15:0] pal_rd_dst_rgb565;
    logic [31:0] pal_rd_w_q;
    logic [33:0] pal_rd_ray_scale_q16;

    /* Palette-lookup register (plr) stage: second half of the
     * pipelined palette read. Captures palette[pal_rd_src_addr] and
     * palette[pal_rd_fog_addr] so fog0 sees RGB values that arrive
     * on the same cycle as every other fog0 input. See PROJECT_NOTES.md
     * for the reasoning. (Prefix avoids colliding with the existing
     * CSR-side pal_addr register used to auto-increment palette
     * writes.) */
    logic        plr_valid;
    logic        plr_pass;
    logic        plr_ztest;
    logic  [1:0] plr_alpha;
    logic        plr_fog;
    logic [15:0] plr_addr;
    logic [15:0] plr_z;
    logic [23:0] plr_src_rgb;
    logic [15:0] plr_dst_rgb565;
    logic [23:0] plr_fog_rgb;
    logic [31:0] plr_w_q;
    logic [33:0] plr_ray_scale_q16;

    logic        fog0_valid;
    logic        fog0_pass;
    logic        fog0_ztest;
    logic  [1:0] fog0_alpha;
    logic        fog0_fog;
    logic [15:0] fog0_addr;
    logic [15:0] fog0_z;
    logic [15:0] fog0_src_rgb565;
    logic [15:0] fog0_dst_rgb565;
    logic [15:0] fog0_fog_rgb565;
    logic [31:0] fog0_w_q;
    logic [33:0] fog0_ray_scale_q16;
    logic        fog1_valid;
    logic        fog1_pass;
    logic        fog1_ztest;
    logic  [1:0] fog1_alpha;
    logic        fog1_fog;
    logic [15:0] fog1_addr;
    logic [15:0] fog1_z;
    logic [15:0] fog1_src_rgb565;
    logic [15:0] fog1_dst_rgb565;
    logic [15:0] fog1_fog_rgb565;
    logic [15:0] fog1_radial_q8_8;
    logic        commit_valid;
    logic        commit_pass;
    logic        commit_ztest;
    logic [15:0] commit_addr;
    logic [15:0] commit_z;
    logic [15:0] commit_color;

    /* Odd lane for the 2 px/cycle raster pipe. Existing unsuffixed
     * registers are lane0/even; `_o` registers carry lane1/odd. */
    logic        pipe0_valid_o;
    logic        pipe0_inside_o;
    logic        pipe0_ztest_o;
    logic        pipe0_textured_o;
    logic        pipe0_alpha_key_o;
    logic  [1:0] pipe0_alpha_o;
    logic        pipe0_fog_o;
    logic  [1:0] pipe0_light_bank_o;
    logic  [7:0] pipe0_tex_or_color_o;
    logic [15:0] pipe0_addr_o;
    logic [15:0] pipe0_z_o;
    logic  [9:0] pipe0_x_o;
    logic  [8:0] pipe0_y_o;
    logic signed [31:0] pipe0_uw_q_o;
    logic signed [31:0] pipe0_vw_q_o;
    logic [31:0] pipe0_iw_q_o;
    logic        recip0_valid_o;
    logic        recip0_inside_o;
    logic        recip0_ztest_o;
    logic        recip0_textured_o;
    logic        recip0_alpha_key_o;
    logic  [1:0] recip0_alpha_o;
    logic        recip0_fog_o;
    logic  [1:0] recip0_light_bank_o;
    logic  [7:0] recip0_tex_or_color_o;
    logic [15:0] recip0_addr_o;
    logic [15:0] recip0_z_o;
    logic  [9:0] recip0_x_o;
    logic  [8:0] recip0_y_o;
    logic signed [31:0] recip0_uw_q_o;
    logic signed [31:0] recip0_vw_q_o;
    logic        recip0_iw_zero_o;
    logic  [5:0] recip0_iw_msb_o;
    logic [31:0] recip0_iw_norm_q_o;
    logic        recip1_valid_o;
    logic        recip1_inside_o;
    logic        recip1_ztest_o;
    logic        recip1_textured_o;
    logic        recip1_alpha_key_o;
    logic  [1:0] recip1_alpha_o;
    logic        recip1_fog_o;
    logic  [1:0] recip1_light_bank_o;
    logic  [7:0] recip1_tex_or_color_o;
    logic [15:0] recip1_addr_o;
    logic [15:0] recip1_z_o;
    logic [15:0] recip1_z_ref_o;
    logic [15:0] recip1_dst_rgb565_o;
    logic  [9:0] recip1_x_o;
    logic  [8:0] recip1_y_o;
    logic signed [31:0] recip1_uw_q_o;
    logic signed [31:0] recip1_vw_q_o;
    logic        recip1_iw_zero_o;
    logic  [5:0] recip1_iw_msb_o;
    logic  [5:0] recip1_iw_lut_frac_o;
    logic [31:0] recip1_w_norm_lo_o;
    logic [31:0] recip1_w_norm_hi_o;
    logic        recip2_valid_o;
    logic        recip2_inside_o;
    logic        recip2_ztest_o;
    logic        recip2_textured_o;
    logic        recip2_alpha_key_o;
    logic  [1:0] recip2_alpha_o;
    logic        recip2_fog_o;
    logic  [1:0] recip2_light_bank_o;
    logic  [7:0] recip2_tex_or_color_o;
    logic [15:0] recip2_addr_o;
    logic [15:0] recip2_z_o;
    logic [15:0] recip2_z_ref_o;
    logic [15:0] recip2_dst_rgb565_o;
    logic  [9:0] recip2_x_o;
    logic  [8:0] recip2_y_o;
    logic signed [31:0] recip2_uw_q_o;
    logic signed [31:0] recip2_vw_q_o;
    logic        recip2_iw_zero_o;
    logic  [5:0] recip2_iw_msb_o;
    logic [31:0] recip2_w_norm_q_o;
    logic        pipe1_valid_o;
    logic        pipe1_inside_o;
    logic        pipe1_ztest_o;
    logic        pipe1_textured_o;
    logic        pipe1_alpha_key_o;
    logic  [1:0] pipe1_alpha_o;
    logic        pipe1_fog_o;
    logic  [1:0] pipe1_light_bank_o;
    logic  [7:0] pipe1_tex_or_color_o;
    logic [15:0] pipe1_addr_o;
    logic [15:0] pipe1_z_o;
    logic [15:0] pipe1_z_ref_o;
    logic [15:0] pipe1_dst_rgb565_o;
    logic  [9:0] pipe1_x_o;
    logic  [8:0] pipe1_y_o;
    logic signed [31:0] pipe1_uw_q_o;
    logic signed [31:0] pipe1_vw_q_o;
    logic [31:0] pipe1_w_q_o;
    logic        tex0_valid_o;
    logic        tex0_inside_o;
    logic        tex0_ztest_o;
    logic        tex0_textured_o;
    logic        tex0_alpha_key_o;
    logic  [1:0] tex0_alpha_o;
    logic        tex0_fog_o;
    logic  [1:0] tex0_light_bank_o;
    logic  [7:0] tex0_tex_or_color_o;
    logic [15:0] tex0_addr_o;
    logic [15:0] tex0_z_o;
    logic [15:0] tex0_z_ref_o;
    logic [15:0] tex0_dst_rgb565_o;
    logic  [9:0] tex0_x_o;
    logic  [8:0] tex0_y_o;
    logic [31:0] tex0_w_q_o;
    logic signed [63:0] tex0_u_prod_o;
    logic signed [63:0] tex0_v_prod_o;
    logic        pipe2_valid_o;
    logic        pipe2_inside_o;
    logic        pipe2_ztest_o;
    logic        pipe2_textured_o;
    logic        pipe2_alpha_key_o;
    logic  [1:0] pipe2_alpha_o;
    logic        pipe2_fog_o;
    logic  [1:0] pipe2_light_bank_o;
    logic  [7:0] pipe2_tex_or_color_o;
    logic [15:0] pipe2_addr_o;
    logic [15:0] pipe2_z_o;
    logic [15:0] pipe2_z_ref_o;
    logic [15:0] pipe2_dst_rgb565_o;
    logic  [9:0] pipe2_x_o;
    logic  [8:0] pipe2_y_o;
    logic [31:0] pipe2_w_q_o;
    logic [13:0] pipe2_tex_addr_o;
    logic        draw_pipe_valid_o;
    logic        draw_pipe_inside_o;
    logic        draw_pipe_ztest_o;
    logic        draw_pipe_textured_o;
    logic        draw_pipe_alpha_key_o;
    logic  [1:0] draw_pipe_alpha_o;
    logic        draw_pipe_fog_o;
    logic  [1:0] draw_pipe_light_bank_o;
    logic  [7:0] draw_pipe_tex_or_color_o;
    logic [15:0] draw_pipe_addr_o;
    logic [15:0] draw_pipe_z_o;
    logic [15:0] draw_pipe_z_ref_o;
    logic [15:0] draw_pipe_dst_rgb565_o;
    logic  [9:0] draw_pipe_x_o;
    logic  [8:0] draw_pipe_y_o;
    logic [31:0] draw_pipe_w_q_o;
    logic        pal_rd_valid_o;
    logic        pal_rd_pass_o;
    logic        pal_rd_ztest_o;
    logic  [1:0] pal_rd_alpha_o;
    logic        pal_rd_fog_o;
    logic [15:0] pal_rd_addr_o;
    logic [15:0] pal_rd_z_o;
    logic  [7:0] pal_rd_src_addr_o;
    logic  [7:0] pal_rd_fog_addr_o;
    logic [15:0] pal_rd_dst_rgb565_o;
    logic [31:0] pal_rd_w_q_o;
    logic [33:0] pal_rd_ray_scale_q16_o;
    logic        plr_valid_o;
    logic        plr_pass_o;
    logic        plr_ztest_o;
    logic  [1:0] plr_alpha_o;
    logic        plr_fog_o;
    logic [15:0] plr_addr_o;
    logic [15:0] plr_z_o;
    logic [23:0] plr_src_rgb_o;
    logic [15:0] plr_dst_rgb565_o;
    logic [23:0] plr_fog_rgb_o;
    logic [31:0] plr_w_q_o;
    logic [33:0] plr_ray_scale_q16_o;
    logic        fog0_valid_o;
    logic        fog0_pass_o;
    logic        fog0_ztest_o;
    logic  [1:0] fog0_alpha_o;
    logic        fog0_fog_o;
    logic [15:0] fog0_addr_o;
    logic [15:0] fog0_z_o;
    logic [15:0] fog0_src_rgb565_o;
    logic [15:0] fog0_dst_rgb565_o;
    logic [15:0] fog0_fog_rgb565_o;
    logic [31:0] fog0_w_q_o;
    logic [33:0] fog0_ray_scale_q16_o;
    logic        fog1_valid_o;
    logic        fog1_pass_o;
    logic        fog1_ztest_o;
    logic  [1:0] fog1_alpha_o;
    logic        fog1_fog_o;
    logic [15:0] fog1_addr_o;
    logic [15:0] fog1_z_o;
    logic [15:0] fog1_src_rgb565_o;
    logic [15:0] fog1_dst_rgb565_o;
    logic [15:0] fog1_fog_rgb565_o;
    logic [15:0] fog1_radial_q8_8_o;
    logic        commit_valid_o;
    logic        commit_pass_o;
    logic        commit_ztest_o;
    logic [15:0] commit_addr_o;
    logic [15:0] commit_z_o;
    logic [15:0] commit_color_o;
    // tex_rd_data is driven combinationally by voxel_texture_rom's
    // registered output. The ROM takes pipe2_tex_addr on cycle T and
    // presents mem[pipe2_tex_addr[T]] on cycle T+1, which is the same
    // 1-cycle latency the draw_pipe stage expects (see the instance
    // below and the voxel_texture_rom module header for rationale).
    wire   [7:0] tex_rd_data;
    wire   [7:0] tex_rd_data_o;

    logic [10:0] hcount;
    logic  [9:0] vcount;

    logic        scan_visible_now;
    logic [15:0] scan_rgb565_r;
    logic        scan_visible_r;
    logic [15:0] draw_addr;
    logic [15:0] draw_addr_o;
    logic [15:0] fb_back_rd_addr;
    logic [15:0] fb_back_rd_addr_o;
    // fb_back_rd_data is now a wire driven by the ping-pong cache mux
    logic [15:0] fb_wr_addr;
    logic [15:0] fb_wr_data;
    logic        fb_back_wr_en;
    logic [15:0] fb_wr_addr_e;
    logic [15:0] fb_wr_data_e;
    logic        fb_back_wr_en_e;
    logic [15:0] fb_wr_addr_o;
    logic [15:0] fb_wr_data_o;
    logic        fb_back_wr_en_o;
    logic [15:0] z_rd_addr;
    logic [15:0] z_rd_addr_o;
    // z_rd_data is now a wire driven by the ping-pong cache mux
    logic [15:0] z_wr_addr;
    logic [15:0] z_wr_data;
    logic        z_wr_en;
    logic [15:0] z_wr_addr_e;
    logic [15:0] z_wr_data_e;
    logic        z_wr_en_e;
    logic [15:0] z_wr_addr_o;
    logic [15:0] z_wr_data_o;
    logic        z_wr_en_o;

    (* ramstyle = "MLAB, no_rw_check" *) logic [15:0] scan_linebuf0 [0:LINE_WORDS-1];
    (* ramstyle = "MLAB, no_rw_check" *) logic [15:0] scan_linebuf1 [0:LINE_WORDS-1];
    (* ramstyle = "MLAB, no_rw_check" *) logic [15:0] scan_linebuf2 [0:LINE_WORDS-1];
    logic        scan_line0_ready;
    logic        scan_line1_ready;
    logic        scan_line2_ready;
    logic  [8:0] scan_line0_row;
    logic  [8:0] scan_line1_row;
    logic  [8:0] scan_line2_row;
    logic  [1:0] scan_active_bank;
    logic        scan_active_valid;
    logic  [8:0] scan_active_row;
    logic        scan_fill_active;
    logic        scan_fill_armed;
    logic        scan_fill_load_pending;
    logic  [1:0] scan_fill_bank;
    logic  [8:0] scan_fill_row;
    logic [24:0] scan_fill_base_words;
    logic  [9:0] scan_fill_store_idx;
    logic        scan_rd_capture;
    logic [15:0] scan_rgb565_now;
    logic [15:0] scan_late_count;
    logic [24:0] sdram_rd_addr_cfg;
    logic [24:0] sdram_rd_max_addr_cfg;
    logic        sdram_rd_load_pulse;
    /*
     * Some RD_LOADs need a multi-cycle FIFO clear, but stretching every
     * 64-word scanline chunk is expensive. Use the long clear for scanline
     * starts and cache-load starts, where stale tail data can contaminate the
     * first visible chunk; continuation chunks use the normal one-cycle pulse.
     */
    logic        sdram_rd_load_stretch_req;
    logic  [3:0] sdram_rd_load_hold;
    wire         sdram_rd_load_out = sdram_rd_load_pulse ||
                                     (sdram_rd_load_hold != 4'd0);

    logic  [2:0] cache_band_index;
    logic  [2:0] cache_target_band;
    logic  [2:0] band_index_cfg;
    logic        band_begin_pending;
    logic        band_flush_pending;
    logic        cache_valid;
    logic        cache_dirty;
    logic        cache_draw_dirty;
    logic  [7:0] cache_band_valid;
    logic        cache_resume_draw;
    logic        cache_final_flush;
    logic [15:0] cache_maint_addr;
    logic [15:0] cache_pixels_total;
    logic  [9:0] cache_init_x;
    logic  [4:0] cache_init_sky_row_count;
    logic  [7:0] cache_init_sky_palette;
    logic [15:0] cache_words_issued;
    logic [15:0] cache_words_done;
    logic        cache_fetch_inflight;
    logic        cache_flush_load_pending;
    logic  [7:0] cache_drain_count;
    logic        cache_word_pending_valid;
    logic [15:0] cache_word_pending;
    logic        cache_load_is_z;
    logic        cache_rd_capture;
    logic [24:0] sdram_wr_addr_cfg;
    logic [24:0] sdram_wr_max_addr_cfg;
    logic        sdram_wr_load_pulse;

    /* ── Ping-pong band cache ─────────────────────────────── */
    logic        draw_cache_sel;           // 0 = A active, 1 = B active
    logic        flush_active;             // background flush running
    logic [15:0] flush_maint_addr;         // read address into inactive cache
    logic [15:0] flush_pixels_total;       // pixels in the flushing band
    logic [15:0] flush_words_issued;       // cache reads / generated words issued
    logic [15:0] flush_words_done;         // words pushed to SDRAM wr FIFO
    logic        flush_fetch_inflight;     // one-cycle read latency pending
    logic        flush_word_pending_valid; // captured pixel waiting for SDRAM push
    logic [15:0] flush_word_pending;       // the captured pixel value
    logic        flush_load_pending;       // kick the SDRAM write burst
    logic  [7:0] flush_drain_count;        // wait after WR FIFO empty for final SDRAM burst
    logic  [2:0] flush_band_index;         // which band is being flushed
    logic [24:0] flush_sdram_wr_addr;      // SDRAM write base for flush
    logic [24:0] flush_sdram_wr_max_addr;  // SDRAM write end for flush
    logic        flush_cache_sel;          // which cache to flush (0=A, 1=B)
    logic        flush_generated_sky;      // source is sky palette, not local cache
    logic  [9:0] flush_sky_x;
    logic  [4:0] flush_sky_row_count;
    logic  [7:0] flush_sky_palette;

    /*
     * Per-cache, per-bank read/write signals. Each cache (fb_A, fb_B,
     * z_A, z_B) is now a banked SDP RAM exposing two physical banks
     * keyed on linear addr[0]: `_e` is the even bank and `_o` is the
     * odd bank. For 1 px/cycle paths today, the cache port driver
     * fans the unified addr/data/en to both banks of the active cache
     * and qualifies each `wr_en_*` by linear addr[0]. The unified
     * `rd_data` view that the rest of the design consumes is then
     * recovered by muxing `rd_data_e` / `rd_data_o` with a 1-cycle
     * delayed copy of the LSB of the read address (`*_rd_sel_q`) —
     * the same trick the old in-wrapper mux performed, hoisted out so
     * the per-bank ports can be driven independently in step 4b/c.
     */
    logic [15:0] fb_A_e_rd_addr, fb_A_o_rd_addr;
    logic [15:0] fb_A_e_rd_data, fb_A_o_rd_data;
    logic [15:0] fb_A_e_wr_addr, fb_A_o_wr_addr;
    logic [15:0] fb_A_e_wr_data, fb_A_o_wr_data;
    logic        fb_A_e_wr_en,   fb_A_o_wr_en;
    logic [15:0] fb_B_e_rd_addr, fb_B_o_rd_addr;
    logic [15:0] fb_B_e_rd_data, fb_B_o_rd_data;
    logic [15:0] fb_B_e_wr_addr, fb_B_o_wr_addr;
    logic [15:0] fb_B_e_wr_data, fb_B_o_wr_data;
    logic        fb_B_e_wr_en,   fb_B_o_wr_en;
    logic [15:0] z_A_e_rd_addr,  z_A_o_rd_addr;
    logic [15:0] z_A_e_rd_data,  z_A_o_rd_data;
    logic [15:0] z_A_e_wr_addr,  z_A_o_wr_addr;
    logic [15:0] z_A_e_wr_data,  z_A_o_wr_data;
    logic        z_A_e_wr_en,    z_A_o_wr_en;
    logic [15:0] z_B_e_rd_addr,  z_B_o_rd_addr;
    logic [15:0] z_B_e_rd_data,  z_B_o_rd_data;
    logic [15:0] z_B_e_wr_addr,  z_B_o_wr_addr;
    logic [15:0] z_B_e_wr_data,  z_B_o_wr_data;
    logic        z_B_e_wr_en,    z_B_o_wr_en;
    /* 1-cycle-delayed LSB of each cache's read address. Equal to what
     * the old in-wrapper rd_sel_q used to be. */
    logic        fb_A_rd_sel_q, fb_B_rd_sel_q;
    logic        z_A_rd_sel_q,  z_B_rd_sel_q;
    /* Reconstructed unified per-cache rd_data, consumed by the existing
     * fb_back_rd_data / z_rd_data muxes. */
    logic [15:0] fb_A_rd_data, fb_B_rd_data;
    logic [15:0] z_A_rd_data,  z_B_rd_data;

    logic [17:0] sdram_powerup_counter;
    logic [15:0] sdram_init_wait_counter;
    logic        sdram_ctrl_reset_n;
    logic        sdram_ready;
    logic        sdram_ctrl_clk;
    logic        sdram_wr_full;
    logic [15:0] sdram_wr_use;
    logic [15:0] sdram_rd_data;
    logic        sdram_rd_empty;
    logic [15:0] sdram_rd_use;
    logic  [1:0] dram_cs_n_bus;

    logic vga_vs_d;

    wire wr = chipselect & write;
    wire ctrl_clear_write = wr && (address == ADDR_CONTROL) && writedata[3];
    wire fifo_push_req = wr && (address >= ADDR_FIFO_LO) && (address < ADDR_FIFO_HI) && !fifo_full;
    wire desc_has_uv = desc_flags[FLAG_TEX_BIT];
    wire [5:0] fetch_target_words = ((fetch_count >= 6'd16) && desc_has_uv) ? 6'd32 : 6'd16;
    wire fifo_pop_req = (state == ST_FETCH) && (fetch_count < fetch_target_words) && !fifo_empty;
    /*
     * band_flush_pending is intentionally excluded from engine_busy.
     * The background flush runs independently via flush_active; including
     * the pending flag in BSY made the driver stall in END_BAND until the
     * previous band's flush finished draining to SDRAM, which serialised
     * every band and halved the frame rate on sky/ground views.  The FSM
     * priority chain (flush before begin) and band_begin_cache_available
     * gate handle the ordering correctly without blocking the driver.
     */
    wire engine_busy = (state != ST_IDLE) || clear_pending ||
                       band_begin_pending;
    wire vsync_pulse = vga_vs_d & ~VGA_VS;
    wire [24:0] extmem_front_base_words = extmem_front_base[25:1];
    wire [24:0] extmem_back_base_words  = extmem_back_base[25:1];
    wire [24:0] extmem_z_base_words     = DEFAULT_EXTMEM_Z_BASE[25:1];
    wire [24:0] display_base_words      = display_sel ? extmem_back_base_words : extmem_front_base_words;
    wire [24:0] copy_target_base_words  = copy_target_sel ? extmem_back_base_words : extmem_front_base_words;
    /*
     * vcount counts 0..524 (VTOTAL-1) but scan_current_row is only 9 bits, so
     * a naive vcount[8:0] wraps at vcount=512 — during the back porch the
     * scanout consumer would see scan_current_row=0..12 and race the advance
     * logic ahead, popping ~13 prefetched rows before the visible frame even
     * starts. That shows up as black at the top (source rows 0..12 gone) and
     * black at the bottom (prefetcher caps at row 479, so active_row stalls
     * there for the last 13 visible vcounts). Pin scan_current_row to 0 outside
     * the active region so neither the comparator nor the +1 target lookahead
     * fires while vcount is past 479.
     */
    wire        vcount_visible          = (vcount < 10'd480);
    wire [8:0]  scan_current_row        = vcount_visible ? vcount[8:0] : 9'd0;
    wire        scan_hblank_window      = vcount_visible && (hcount >= 11'd1280);
    wire        scan_hblank_start       = vcount_visible && (hcount == 11'd1280);
    wire [8:0]  scan_target_row         = scan_hblank_window ?
                                          (scan_current_row + 9'd1) : scan_current_row;
    wire        scan_target_valid       = scan_target_row < 9'd480;
    wire [8:0]  scan_immediate_next_row = scan_current_row + 9'd1;
    wire        scan_immediate_next_valid =
        vcount_visible && (scan_current_row < 9'd479);
    wire [8:0]  scan_far_next_row       = scan_current_row + 9'd2;
    wire        scan_far_next_valid     =
        vcount_visible && (scan_current_row < 9'd478);
    wire [9:0]  scan_current_x          = hcount[10:1];
    wire        scan_current_x_valid    = (scan_current_x < 10'd640);
    wire [8:0]  scan_active_next_row    = scan_active_row + 9'd1;
    wire        scan_active_bank_ready  = (scan_active_bank == 2'd0) ? scan_line0_ready :
                                          (scan_active_bank == 2'd1) ? scan_line1_ready :
                                                                       scan_line2_ready;
    wire [8:0]  scan_active_bank_row    = (scan_active_bank == 2'd0) ? scan_line0_row :
                                          (scan_active_bank == 2'd1) ? scan_line1_row :
                                                                       scan_line2_row;
    /*
     * Drive scanout prefetch from the VGA row, not from the last active
     * linebuffer row. If scanout ever falls behind, active-row-relative
     * prefetch keeps chasing the stale row and the monitor repeats bands.
     */
    wire        scan_current_line_ready =
        (scan_line0_ready && (scan_line0_row == scan_current_row)) ||
        (scan_line1_ready && (scan_line1_row == scan_current_row)) ||
        (scan_line2_ready && (scan_line2_row == scan_current_row));
    wire        scan_target_line_ready =
        !scan_target_valid ||
        (scan_line0_ready && (scan_line0_row == scan_target_row)) ||
        (scan_line1_ready && (scan_line1_row == scan_target_row)) ||
        (scan_line2_ready && (scan_line2_row == scan_target_row));
    wire        scan_immediate_next_line_ready =
        !scan_immediate_next_valid ||
        (scan_line0_ready && (scan_line0_row == scan_immediate_next_row)) ||
        (scan_line1_ready && (scan_line1_row == scan_immediate_next_row)) ||
        (scan_line2_ready && (scan_line2_row == scan_immediate_next_row));
    wire        scan_far_next_line_ready =
        !scan_far_next_valid ||
        (scan_line0_ready && (scan_line0_row == scan_far_next_row)) ||
        (scan_line1_ready && (scan_line1_row == scan_far_next_row)) ||
        (scan_line2_ready && (scan_line2_row == scan_far_next_row));
    wire        scan_target_or_active_ready =
        !scan_active_valid || scan_target_line_ready;
    wire        scan_prefetch_recover_current =
        vcount_visible && !scan_current_line_ready;
    wire        scan_prefetch_need_target =
        vcount_visible && scan_target_valid &&
        scan_current_line_ready && !scan_target_line_ready;
    wire        scan_prefetch_need_next =
        vcount_visible && scan_current_line_ready &&
        scan_immediate_next_valid && !scan_immediate_next_line_ready;
    wire        scan_prefetch_need_far =
        vcount_visible && scan_current_line_ready &&
        scan_immediate_next_line_ready &&
        scan_far_next_valid && !scan_far_next_line_ready;
    wire [8:0]  scan_prefetch_row =
        scan_prefetch_recover_current ? scan_current_row :
        scan_prefetch_need_target     ? scan_target_row  :
        scan_prefetch_need_next       ? scan_immediate_next_row :
                                        scan_far_next_row;
    wire [24:0] scan_prefetch_base_words =
        display_base_words +
        {7'd0, scan_prefetch_row, 9'd0} +
        {9'd0, scan_prefetch_row, 7'd0};
    wire        scan_prefetch_valid     =
        vcount_visible && scan_active_valid &&
        (scan_prefetch_recover_current ||
         scan_prefetch_need_target ||
         scan_prefetch_need_next ||
         scan_prefetch_need_far);
    wire        scan_prefetch_ready     =
        (scan_line0_ready && (scan_line0_row == scan_prefetch_row)) ||
        (scan_line1_ready && (scan_line1_row == scan_prefetch_row)) ||
        (scan_line2_ready && (scan_line2_row == scan_prefetch_row));
    /* Protect any bank holding a row scanout will need before it can be
     * legitimately reloaded: current (showing now), target (next at hblank),
     * immediate_next (current+1), and far_next (current+2). Without far_next
     * protection a 2-line prefetch could land in bank X and then be evicted
     * by the next prefetch tick — wasting the SDRAM burst and increasing
     * the chance scanout falls behind. */
    wire        scan_bank0_protected    =
        scan_line0_ready &&
        ((scan_line0_row == scan_current_row) ||
         (scan_target_valid &&
          (scan_line0_row == scan_target_row)) ||
         (scan_immediate_next_valid &&
          (scan_line0_row == scan_immediate_next_row)) ||
         (scan_far_next_valid &&
          (scan_line0_row == scan_far_next_row)));
    wire        scan_bank1_protected    =
        scan_line1_ready &&
        ((scan_line1_row == scan_current_row) ||
         (scan_target_valid &&
          (scan_line1_row == scan_target_row)) ||
         (scan_immediate_next_valid &&
          (scan_line1_row == scan_immediate_next_row)) ||
         (scan_far_next_valid &&
          (scan_line1_row == scan_far_next_row)));
    wire        scan_bank2_protected    =
        scan_line2_ready &&
        ((scan_line2_row == scan_current_row) ||
         (scan_target_valid &&
          (scan_line2_row == scan_target_row)) ||
         (scan_immediate_next_valid &&
          (scan_line2_row == scan_immediate_next_row)) ||
         (scan_far_next_valid &&
          (scan_line2_row == scan_far_next_row)));
    wire        scan_bank0_free         = (scan_active_bank != 2'd0) &&
                                          !scan_bank0_protected;
    wire        scan_bank1_free         = (scan_active_bank != 2'd1) &&
                                          !scan_bank1_protected;
    wire        scan_bank2_free         = (scan_active_bank != 2'd2) &&
                                          !scan_bank2_protected;
    wire [1:0]  scan_prefetch_bank      = scan_bank0_free ? 2'd0 :
                                          scan_bank1_free ? 2'd1 :
                                          scan_bank2_free ? 2'd2 : 2'd3;
    wire        scan_read_idle          = !scan_fill_active && !scan_fill_armed &&
                                          !scan_fill_load_pending &&
                                          sdram_rd_empty;
    wire        cache_flush_state       = (state == ST_CACHE_FLUSH_COLOR);
    wire        cache_load_state        = (state == ST_CACHE_LOAD_COLOR || state == ST_CACHE_LOAD_Z);
    /*
     * Drain phase after cache_load: the SDRAM controller auto-bursts in
     * 64-word chunks, so the FIFO can hold up to 64 stale words after the
     * last useful pixel for a band. Without an explicit drain, the next
     * RD_LOAD races with those residuals and the new burst's first chunk
     * (~64 pixels = 1/10 of a 640-wide line) lands wherever a scanline
     * happens to capture, producing the left-edge streak. Drain pops every
     * residual word and waits for sdram_rd_empty to be stable for
     * COPY_DRAIN_CYCLES before allowing the next read or scan_fill burst.
     */
    wire        cache_drain_state       = (state == ST_CACHE_DRAIN_COLOR ||
                                           state == ST_CACHE_DRAIN_Z);
    wire        cache_init_state        = (state == ST_CACHE_INIT);
    wire        cache_maint_state       = cache_flush_state || cache_load_state || cache_init_state;
    /*
     * "Rasterizer, sky-skip patch, or cache-maintenance currently needs the
     * active cache port." Used by the ping-pong port mux to decide whether
     * the flush controller may take ownership when draw_cache_sel == flush_cache_sel.
     * Without this gate, the FINAL band never gets a follow-up BEGIN_BAND
     * to toggle draw_cache_sel away, so the flush reads through the
     * rasterizer port (fb_back_rd_addr = pipe0_addr — stale) and writes a
     * single repeated pixel for the entire bottom band. Keeping the
     * rasterizer-priority branch live only while the rasterizer is
     * actually using the cache lets the flush take over once the
     * rasterizer/cache-maintenance pipeline is idle, while still
     * preserving the intermediate-band ping-pong overlap.
     */
    wire        cache_used_by_main      = cache_sky_patch_state ||
                                          (state == ST_DRAW) ||
                                          (state == ST_DRAW_FLUSH) ||
                                          cache_init_state ||
                                          cache_load_state ||
                                          cache_flush_state;
    /*
     * The `!band_flush_pending` clause keeps cache_band_index from being
     * clobbered while a previous band's END_BAND is still queued in
     * band_flush_pending. Without it, when the prior flush is still
     * draining (flush_active=1) and SW pipelines END_N → BEGIN_(N+1), the
     * begin branch can fire first, overwrite cache_band_index with N+1,
     * and then the leftover band_flush_pending fires for the wrong band —
     * band N never lands in SDRAM. Hold BEGIN until the priority chain
     * has had one cycle in ST_IDLE to commit the queued flush into its
     * own flush_*_index registers; then BEGIN can pipeline freely.
     */
    wire        band_begin_cache_available =
        !band_flush_pending &&
        (!flush_active || flush_generated_sky || ((~draw_cache_sel) != flush_cache_sel));
    wire        scan_vsync_read_req     = vsync_pulse && sdram_ready &&
                                          (copy_complete_pending || display_valid);
    wire        scan_next_read_req      = !cache_load_state && display_valid &&
                                          sdram_ready && scan_active_valid &&
                                          vcount_visible &&
                                          (!scan_current_line_ready ||
                                           !scan_target_line_ready);
    wire        scan_read_start_req     = scan_vsync_read_req || scan_next_read_req;
    /*
     * Defense-in-depth: never spawn a scan-fill burst while the SDRAM RD
     * FIFO still has residuals. The cache_load drain phase should keep
     * this from being load-bearing, but gating here means a single missed
     * drain elsewhere can't poison linebuf[0..63] with stale words.
     */
    wire        scan_prefetch_req       = !scan_fill_active && display_valid &&
                                          sdram_ready && scan_active_valid &&
                                          scan_prefetch_valid && !scan_prefetch_ready &&
                                          (scan_prefetch_bank != 2'd3) &&
                                          sdram_rd_empty;
    /*
     * Band flushes need active-video SDRAM time to avoid crawling at porch-only
     * bandwidth. They are writes, so allow them once scanout has both the next
     * line and the prefetch target resident, but stop launching active-visible
     * bursts late in the line so the next scanline fill has recovery room.
     * Keep SDRAM read-side cache loads restricted to blanking; those are the
     * risky path for RD FIFO tail data.
     */
    /*
     * Gates background band-cache flushes (writes to SDRAM) on scanout having
     * enough headroom. We require target + immediate_next resident, but NOT
     * far_next: the 2-line prefetch is best-effort and is now bank-protected
     * once it lands, so it doesn't need to also block writes. Including
     * far_next here starved world-block flushes whenever scanout was 2 lines
     * behind — sky and HUD bands snuck through during vsync, but the bulk of
     * world geometry never made it back to SDRAM, so ground/blocks vanished.
     */
    wire        scan_prefetch_margin_ready =
        scan_target_or_active_ready &&
        scan_immediate_next_line_ready &&
        (!scan_prefetch_valid || scan_prefetch_ready);
    wire        scan_active_write_window = vcount_visible &&
                                           (hcount < ACTIVE_WRITE_END_HCOUNT);
    wire        scanout_write_slack     = !display_valid ||
                                          (scan_read_idle &&
                                           !scan_prefetch_req &&
                                           (!VGA_BLANK_n ||
                                            (scan_prefetch_margin_ready &&
                                             scan_active_write_window)));
    wire        scanout_read_slack      = !display_valid ||
                                          (scan_read_idle &&
                                           !scan_prefetch_req &&
                                           !VGA_BLANK_n);
    wire        scanout_read_load_req   = scan_vsync_read_req ||
                                          scan_fill_load_pending ||
                                          scan_prefetch_req;
    wire        bg_flush_wr_load_req    = flush_active && flush_load_pending &&
                                          scanout_write_slack &&
                                          !scanout_read_load_req;
    wire        bg_flush_stream_active  = flush_active && !flush_load_pending;
    wire        cache_read_start_ok     = scanout_read_slack && scan_read_idle &&
                                          !scan_read_start_req && !scan_prefetch_req;
    wire        scan_visible_data_ready = display_valid && sdram_ready &&
                                          scan_active_bank_ready &&
                                          (scan_active_bank_row == scan_active_row);
    wire [9:0]  scan_fill_words_complete =
        scan_fill_store_idx + (scan_rd_capture ? 10'd1 : 10'd0);
    wire        scan_fill_line_done =
        scan_rd_capture && (scan_fill_words_complete == LINE_WORDS_10);
    wire        scan_fill_chunk_done =
        scan_rd_capture && (scan_fill_words_complete[5:0] == 6'd0);
    /* ── SDRAM write push: serves both main-FSM flush and background flush ── */
    wire        main_flush_wr_push = cache_flush_state && cache_word_pending_valid &&
                                     !sdram_wr_full && scanout_write_slack;
    /*
     * Tier-1 arbitration: bg flush pushes into the WR FIFO independent of
     * scanout_write_slack. The FIFO push is internal (M10K) and uses no
     * external SDRAM bus cycles — the bus only sees writes when WR_LENGTH
     * fires below, and that signal is still gated by scanout_write_slack.
     * !sdram_wr_full is the hard backpressure gate; combined with the
     * raised COPY_WR_FIFO_HIGH_WATER, this lets bg flush stage during
     * scanout reads and drain in burst-ready chunks during write windows.
     */
    wire        bg_flush_wr_push   = bg_flush_stream_active && flush_word_pending_valid &&
                                     !cache_flush_state &&
                                     !sdram_wr_full;
    wire        sdram_wr_push      = main_flush_wr_push || bg_flush_wr_push;

    wire [15:0] cache_load_words_requested =
        cache_words_done + (cache_rd_capture ? 16'd1 : 16'd0);
    wire        cache_rd_pop = cache_load_state && !sdram_rd_empty &&
                               (cache_load_words_requested < cache_pixels_total);
    /*
     * Suppress pops while scan_fill_armed is asserted: the burst was just
     * programmed via sdram_rd_load_pulse but the SDRAM controller has not yet
     * begun delivering data for it, so any !sdram_rd_empty in this window is
     * stale residual from the prior burst that would otherwise contaminate
     * scan_linebuf[0]. scan_fill_armed clears at lines ~1995 the cycle after
     * sdram_rd_empty deasserts (new data arrived), at which point pops resume.
     */
    wire        scan_rd_pop = !cache_load_state && !cache_drain_state &&
                              scan_fill_active && !scan_fill_armed && !sdram_rd_empty &&
                              (scan_fill_store_idx + (scan_rd_capture ? 10'd1 : 10'd0) <
                               LINE_WORDS_10) &&
                              !scan_fill_chunk_done;
    /*
     * During cache-load drain, pop without capturing anywhere. The SDRAM
     * controller's RD_LENGTH is already 0 (cache_load_state went false at
     * drain entry), so no new bursts launch — drain just empties the
     * residuals from the last in-flight burst.
     */
    wire        drain_rd_pop = cache_drain_state && !sdram_rd_empty;
    wire        sdram_rd_pop = scan_rd_pop || cache_rd_pop || drain_rd_pop;
    wire        cache_can_issue_read =
        cache_flush_state &&
        (cache_words_issued < cache_pixels_total) &&
        !cache_fetch_inflight &&
        scanout_write_slack &&
        (sdram_wr_use[8:0] < COPY_WR_FIFO_HIGH_WATER) &&
        (!cache_word_pending_valid || main_flush_wr_push);
    wire        cache_issue_read = cache_can_issue_read;

    /* Background flush controller can issue a read from the inactive cache.
     * Tier-1 arbitration: dropped scanout_write_slack from issue gate so the
     * cache→FIFO pipeline keeps moving during scanout reads. Backpressure is
     * preserved by sdram_wr_use < COPY_WR_FIFO_HIGH_WATER. */
    wire        flush_can_issue_read =
        bg_flush_stream_active &&
        !flush_generated_sky &&
        !cache_flush_state &&
        (flush_words_issued < flush_pixels_total) &&
        !flush_fetch_inflight &&
        (sdram_wr_use[8:0] < COPY_WR_FIFO_HIGH_WATER) &&
        (!flush_word_pending_valid || bg_flush_wr_push);
    wire        flush_can_issue_sky =
        bg_flush_stream_active &&
        flush_generated_sky &&
        !cache_flush_state &&
        (flush_words_issued < flush_pixels_total) &&
        (sdram_wr_use[8:0] < COPY_WR_FIFO_HIGH_WATER) &&
        (!flush_word_pending_valid || bg_flush_wr_push);

    wire [8:0]  sdram_wr_length_cfg = ((cache_flush_state || bg_flush_stream_active) &&
                                       scanout_write_slack) ?
                                      COPY_BURST_WORDS_9 : 9'd0;
    /*
     * Keep SDRAM reads in 64-word chunks. A full scanline burst can cross the
     * SDRAM row/column boundary; 64-word chunks stay aligned because both the
     * frame line width and SDRAM column size are multiples of 64.
     */
    /*
     * While scan_fill_load_pending is high, the next burst address is being
     * staged but RD_LOAD has not reached Sdram_Control yet. Holding LENGTH at
     * zero for that one cycle prevents the controller from launching a burst
     * at the previous 64-word chunk address.
     */
    wire [8:0]  sdram_rd_length_cfg = ((scan_fill_armed && !scan_fill_load_pending) ||
                                      (cache_load_state && scanout_read_slack)) ?
                                     READ_BURST_WORDS_9 : 9'd0;

    /* ── Ping-pong cache port muxing ─────────────────────────────────────── */
    /* Rasterizer read data from active cache */
    wire [15:0] fb_back_rd_data = draw_cache_sel ? fb_B_rd_data : fb_A_rd_data;
    wire [15:0] z_rd_data       = draw_cache_sel ? z_B_rd_data  : z_A_rd_data;
    wire [15:0] fb_draw_rd_data_e = draw_cache_sel ? fb_B_e_rd_data : fb_A_e_rd_data;
    wire [15:0] fb_draw_rd_data_o = draw_cache_sel ? fb_B_o_rd_data : fb_A_o_rd_data;
    wire [15:0] z_draw_rd_data_e  = draw_cache_sel ? z_B_e_rd_data  : z_A_e_rd_data;
    wire [15:0] z_draw_rd_data_o  = draw_cache_sel ? z_B_o_rd_data  : z_A_o_rd_data;
    /* Flush read data from flush_cache_sel's cache */
    wire [15:0] flush_fb_rd_data = flush_cache_sel ? fb_B_rd_data : fb_A_rd_data;

    wire signed [15:0] desc_x_min_raw = $signed(desc_words[0][15:0]);
    wire signed [15:0] desc_y_min_raw = $signed(desc_words[0][31:16]);
    wire signed [15:0] desc_x_max_raw = $signed(desc_words[1][15:0]);
    wire signed [15:0] desc_y_max_raw = $signed(desc_words[1][31:16]);
    wire [9:0] desc_x_min = clamp_x(desc_x_min_raw);
    wire [9:0] desc_x_max = clamp_x(desc_x_max_raw);
    wire [8:0] desc_y_min = clamp_y(desc_y_min_raw);
    wire [8:0] desc_y_max = clamp_y(desc_y_max_raw);
    wire [15:0] desc_z0 = desc_words[14][15:0];
    wire signed [15:0] desc_dz_dx = $signed(desc_words[14][31:16]);
    wire signed [15:0] desc_dz_dy = $signed(desc_words[15][15:0]);
    wire [7:0] desc_tex_or_color = desc_words[15][23:16];
    wire [7:0] desc_flags = desc_words[15][31:24];
    wire [8:0] desc_band_base_y = band_base_row(cache_band_index);
    wire       desc_redundant_sky_clear =
        sky_gradient_clear_enabled &&
        (desc_flags == 8'd0) &&
        (desc_tex_or_color >= PAL_SKY_GRADIENT_BASE) &&
        (desc_tex_or_color <= PAL_SKY_GRADIENT_LAST) &&
        (desc_x_min == 10'd0) &&
        (desc_x_max == 10'd639);
    wire       desc_band_primer =
        (desc_flags == 8'd0) &&
        (desc_tex_or_color == 8'd0) &&
        (desc_x_min == 10'd0) &&
        (desc_x_max == 10'd0) &&
        (desc_y_min == desc_band_base_y) &&
        (desc_y_max == desc_band_base_y);
    wire       cache_sky_patch_state =
        (state == ST_FETCH) &&
        (fetch_count == fetch_target_words) &&
        desc_redundant_sky_clear;
    // Perspective-correct UV: 9 Q16.16 plane coefficients packed into the
    // second 64-byte block (words 16..24). One_over_w is guaranteed positive
    // at any pixel in front of the near plane — software does not emit quads
    // that touch w <= 0.
    wire signed [31:0] desc_uw_0    = $signed(desc_words[16]);
    wire signed [31:0] desc_uw_dx   = $signed(desc_words[17]);
    wire signed [31:0] desc_uw_dy   = $signed(desc_words[18]);
    wire signed [31:0] desc_vw_0    = $signed(desc_words[19]);
    wire signed [31:0] desc_vw_dx   = $signed(desc_words[20]);
    wire signed [31:0] desc_vw_dy   = $signed(desc_words[21]);
    wire signed [31:0] desc_iw_0    = $signed(desc_words[22]);
    wire signed [31:0] desc_iw_dx   = $signed(desc_words[23]);
    wire signed [31:0] desc_iw_dy   = $signed(desc_words[24]);

    wire [9:0] desc_x_start_even = {desc_x_min[9:1], 1'b0};
    wire [9:0] draw_x_start_even = {draw_x_min[9:1], 1'b0};
    wire [9:0] draw_x_next = draw_x_cur + 10'd1;
    wire signed [10:0] setup_start_x = $signed({1'b0, draw_x_start_even});
    wire signed  [9:0] setup_start_y = $signed({1'b0, draw_y_min});
    /*
     * Keep the whole edge-function expression in signed arithmetic.
     * A manual sign-extension concat here becomes an unsigned operand,
     * which can flip the entire add tree into unsigned math and break
     * negative edge coefficients.
     */
    wire signed [63:0] edge_ax0 = $signed(edge_A[0]) * setup_start_x;
    wire signed [63:0] edge_by0 = $signed(edge_B[0]) * setup_start_y;
    wire signed [63:0] edge_c0  = $signed({{32{edge_C[0][31]}}, edge_C[0]});
    wire signed [63:0] edge_ax1 = $signed(edge_A[1]) * setup_start_x;
    wire signed [63:0] edge_by1 = $signed(edge_B[1]) * setup_start_y;
    wire signed [63:0] edge_c1  = $signed({{32{edge_C[1][31]}}, edge_C[1]});
    wire signed [63:0] edge_ax2 = $signed(edge_A[2]) * setup_start_x;
    wire signed [63:0] edge_by2 = $signed(edge_B[2]) * setup_start_y;
    wire signed [63:0] edge_c2  = $signed({{32{edge_C[2][31]}}, edge_C[2]});
    wire signed [63:0] edge_ax3 = $signed(edge_A[3]) * setup_start_x;
    wire signed [63:0] edge_by3 = $signed(edge_B[3]) * setup_start_y;
    wire signed [63:0] edge_c3  = $signed({{32{edge_C[3][31]}}, edge_C[3]});
    wire signed [63:0] edge_eval0 = edge_ax0 + edge_by0 + edge_c0;
    wire signed [63:0] edge_eval1 = edge_ax1 + edge_by1 + edge_c1;
    wire signed [63:0] edge_eval2 = edge_ax2 + edge_by2 + edge_c2;
    wire signed [63:0] edge_eval3 = edge_ax3 + edge_by3 + edge_c3;
    wire draw_inside = (edge_cur_val[0] >= 0) && (edge_cur_val[1] >= 0) &&
                       (edge_cur_val[2] >= 0) && (edge_cur_val[3] >= 0);
    wire signed [63:0] edge_cur_val_o0 = edge_cur_val[0] + edge_A[0];
    wire signed [63:0] edge_cur_val_o1 = edge_cur_val[1] + edge_A[1];
    wire signed [63:0] edge_cur_val_o2 = edge_cur_val[2] + edge_A[2];
    wire signed [63:0] edge_cur_val_o3 = edge_cur_val[3] + edge_A[3];
    wire draw_inside_o_edge = (edge_cur_val_o0 >= 0) &&
                              (edge_cur_val_o1 >= 0) &&
                              (edge_cur_val_o2 >= 0) &&
                              (edge_cur_val_o3 >= 0);
    wire draw_lane0_in_bounds = (draw_x_cur >= draw_x_min) &&
                                (draw_x_cur <= draw_x_max);
    wire draw_lane1_in_bounds = (draw_x_next >= draw_x_min) &&
                                (draw_x_next <= draw_x_max);
    wire draw_inside_lane0 = draw_inside && draw_lane0_in_bounds;
    wire draw_inside_lane1 = draw_inside_o_edge && draw_lane1_in_bounds;
    wire draw_pair_edge_inside = draw_inside || draw_inside_o_edge;
    wire draw_pair_exited = draw_row_inside && !draw_inside && !draw_inside_o_edge;
    wire draw_pair_last = (draw_x_next >= draw_x_max);
    wire [15:0] draw_z_value = clamp_z(z_cur_val);
    wire signed [47:0] draw_dz_dx_ext = {{32{draw_dz_dx[15]}}, draw_dz_dx};
    wire signed [63:0] draw_uw_dx_ext = {{32{draw_uw_dx[31]}}, draw_uw_dx};
    wire signed [63:0] draw_vw_dx_ext = {{32{draw_vw_dx[31]}}, draw_vw_dx};
    wire signed [63:0] draw_iw_dx_ext = {{32{draw_iw_dx[31]}}, draw_iw_dx};
    wire signed [47:0] draw_z_start_val =
        $signed({32'd0, draw_z0}) - (draw_x_min[0] ? draw_dz_dx_ext : 48'sd0);
    wire signed [63:0] draw_uw_start_val =
        $signed({{32{draw_uw_0[31]}}, draw_uw_0}) -
        (draw_x_min[0] ? draw_uw_dx_ext : 64'sd0);
    wire signed [63:0] draw_vw_start_val =
        $signed({{32{draw_vw_0[31]}}, draw_vw_0}) -
        (draw_x_min[0] ? draw_vw_dx_ext : 64'sd0);
    wire signed [63:0] draw_iw_start_val =
        $signed({{32{draw_iw_0[31]}}, draw_iw_0}) -
        (draw_x_min[0] ? draw_iw_dx_ext : 64'sd0);
    wire [15:0] draw_z_value_o = clamp_z(z_cur_val + draw_dz_dx_ext);
    wire [2:0]  draw_band_index = y_to_band(draw_y_cur);
    wire        draw_cache_hit = cache_valid && (cache_band_index == draw_band_index);
    wire [15:0] draw_x_offset = {6'd0, draw_x_cur} - {6'd0, draw_x_start_even};
    wire [15:0] draw_cache_addr = draw_row_base + draw_x_offset;
    wire [15:0] draw_cache_addr_o = draw_cache_addr + 16'd1;
    wire signed [31:0] draw_uw_q = clamp_s32(uw_cur_val);
    wire signed [31:0] draw_vw_q = clamp_s32(vw_cur_val);
    wire [31:0] draw_iw_q = clamp_pos_u32(iw_cur_val);
    wire signed [31:0] draw_uw_q_o = clamp_s32(uw_cur_val + draw_uw_dx_ext);
    wire signed [31:0] draw_vw_q_o = clamp_s32(vw_cur_val + draw_vw_dx_ext);
    wire [31:0] draw_iw_q_o = clamp_pos_u32(iw_cur_val + draw_iw_dx_ext);
    wire [5:0] pipe0_iw_msb = msb_index32(pipe0_iw_q);
    wire [31:0] pipe0_iw_norm_q = (pipe0_iw_q == 32'd0) ? 32'd0 :
                                  (pipe0_iw_msb >= 6'd16) ?
                                  (pipe0_iw_q >> (pipe0_iw_msb - 6'd16)) :
                                  (pipe0_iw_q << (6'd16 - pipe0_iw_msb));
    wire [5:0] pipe0_iw_msb_o = msb_index32(pipe0_iw_q_o);
    wire [31:0] pipe0_iw_norm_q_o = (pipe0_iw_q_o == 32'd0) ? 32'd0 :
                                    (pipe0_iw_msb_o >= 6'd16) ?
                                    (pipe0_iw_q_o >> (pipe0_iw_msb_o - 6'd16)) :
                                    (pipe0_iw_q_o << (6'd16 - pipe0_iw_msb_o));
    wire [15:0] recip0_iw_phase = recip0_iw_norm_q[15:0];
    wire [10:0] recip0_iw_lut_idx = {1'b0, recip0_iw_phase[15:6]};
    wire [5:0] recip0_iw_lut_frac = recip0_iw_phase[5:0];
    wire [15:0] recip0_iw_phase_o = recip0_iw_norm_q_o[15:0];
    wire [10:0] recip0_iw_lut_idx_o = {1'b0, recip0_iw_phase_o[15:6]};
    wire [5:0] recip0_iw_lut_frac_o = recip0_iw_phase_o[5:0];
    wire [31:0] recip1_w_norm_delta = recip1_w_norm_lo - recip1_w_norm_hi;
    wire [37:0] recip1_w_interp_prod = recip1_w_norm_delta * recip1_iw_lut_frac;
    wire [37:0] recip1_w_interp_step_ext = (recip1_w_interp_prod + 38'd32) >> 6;
    wire [31:0] recip1_w_interp_step = recip1_w_interp_step_ext[31:0];
    wire [31:0] recip1_w_norm_q = recip1_w_norm_lo - recip1_w_interp_step;
    wire [31:0] recip1_w_norm_delta_o = recip1_w_norm_lo_o - recip1_w_norm_hi_o;
    wire [37:0] recip1_w_interp_prod_o = recip1_w_norm_delta_o * recip1_iw_lut_frac_o;
    wire [37:0] recip1_w_interp_step_ext_o = (recip1_w_interp_prod_o + 38'd32) >> 6;
    wire [31:0] recip1_w_interp_step_o = recip1_w_interp_step_ext_o[31:0];
    wire [31:0] recip1_w_norm_q_o = recip1_w_norm_lo_o - recip1_w_interp_step_o;
    wire [31:0] recip2_w_q = recip2_iw_zero ? 32'd0 :
                             (recip2_iw_msb >= 6'd16) ?
                             (recip2_w_norm_q >> (recip2_iw_msb - 6'd16)) :
                             (recip2_w_norm_q << (6'd16 - recip2_iw_msb));
    wire [31:0] recip2_w_q_o = recip2_iw_zero_o ? 32'd0 :
                               (recip2_iw_msb_o >= 6'd16) ?
                               (recip2_w_norm_q_o >> (recip2_iw_msb_o - 6'd16)) :
                               (recip2_w_norm_q_o << (6'd16 - recip2_iw_msb_o));
    wire signed [63:0] pipe1_u_prod = $signed(pipe1_uw_q) * $signed(pipe1_w_q);
    wire signed [63:0] pipe1_v_prod = $signed(pipe1_vw_q) * $signed(pipe1_w_q);
    wire signed [63:0] pipe1_u_prod_o = $signed(pipe1_uw_q_o) * $signed(pipe1_w_q_o);
    wire signed [63:0] pipe1_v_prod_o = $signed(pipe1_vw_q_o) * $signed(pipe1_w_q_o);
    wire tex0_repeat_uv = tex0_tex_or_color[6];
    wire [3:0] tex0_tex_u = texture_coord(tex0_u_prod, tex0_repeat_uv);
    wire [3:0] tex0_tex_v = texture_coord(tex0_v_prod, tex0_repeat_uv);
    wire [13:0] tex0_tex_addr = tex0_textured ?
                                 {tex0_tex_or_color[5:0], tex0_tex_v, tex0_tex_u} :
                                 14'd0;
    wire tex0_repeat_uv_o = tex0_tex_or_color_o[6];
    wire [3:0] tex0_tex_u_o = texture_coord(tex0_u_prod_o, tex0_repeat_uv_o);
    wire [3:0] tex0_tex_v_o = texture_coord(tex0_v_prod_o, tex0_repeat_uv_o);
    wire [13:0] tex0_tex_addr_o = tex0_textured_o ?
                                   {tex0_tex_or_color_o[5:0], tex0_tex_v_o, tex0_tex_u_o} :
                                   14'd0;
    wire  [7:0] draw_pipe_raw_color = draw_pipe_textured ? tex_rd_data : draw_pipe_tex_or_color;
    wire  [7:0] draw_pipe_color = apply_light_bank(draw_pipe_raw_color, draw_pipe_light_bank);
    wire  [7:0] draw_pipe_raw_color_o =
        draw_pipe_textured_o ? tex_rd_data_o : draw_pipe_tex_or_color_o;
    wire  [7:0] draw_pipe_color_o =
        apply_light_bank(draw_pipe_raw_color_o, draw_pipe_light_bank_o);
    wire  [7:0] palette_src_addr = (state == ST_CLEAR) ? 8'd0 : draw_pipe_color;
    wire  [7:0] palette_src_addr_o = (state == ST_CLEAR) ? 8'd0 : draw_pipe_color_o;
    /* Palette reads for the draw pipeline are sampled into plr_* one
     * cycle before fog0 (see the plr_* register block). The
     * rgb888_to_rgb565 conversion is cheap bit slicing, so we leave it
     * as a combinational derivation on the registered palette output
     * and let fog0_src_rgb565 / fog0_fog_rgb565 pick it up on the next
     * cycle. */
    wire [15:0] plr_src_rgb565 = rgb888_to_rgb565(plr_src_rgb);
    wire [15:0] plr_fog_rgb565 = rgb888_to_rgb565(plr_fog_rgb);
    wire [15:0] plr_src_rgb565_o = rgb888_to_rgb565(plr_src_rgb_o);
    wire [15:0] plr_fog_rgb565_o = rgb888_to_rgb565(plr_fog_rgb_o);

    /* Separate combinational reads used by clear/cache-init paths, which write
     * background colors straight to the local band cache and do not go through
     * the draw pipeline. */
    wire [15:0] clear_rgb565 = rgb888_to_rgb565(palette[8'd0]);
    wire        sky_gradient_clear_enabled =
        extmem_ctrl[EXTMEM_SKY_GRADIENT_CLEAR_BIT];
    wire [15:0] cache_init_rgb565 =
        sky_gradient_clear_enabled ?
        rgb888_to_rgb565(palette[cache_init_sky_palette]) :
        clear_rgb565;
    wire draw_pipe_transparent = draw_pipe_textured &&
                                 draw_pipe_alpha_key &&
                                 (draw_pipe_raw_color == 8'd0);
    wire draw_pipe_transparent_o = draw_pipe_textured_o &&
                                   draw_pipe_alpha_key_o &&
                                   (draw_pipe_raw_color_o == 8'd0);
    /*
     * Radial distance estimate: starting from the per-pixel w (linear depth
     * along the camera forward axis) we scale by sqrt(1 + r^2/f^2) where r is
     * the pixel offset from screen center and f is the projection depth in
     * pixels. The square-root is approximated with 1 + 3/8 * r^2/f^2, which
     * is within a couple of percent for our field of view. The final distance
     * is produced in Q8.8 so it can be compared directly against the
     * fog_start_dist / fog_end_dist registers.
     */
    wire signed [11:0] draw_pipe_dx_center =
        $signed({1'b0, draw_pipe_x}) - 12'sd320;
    wire signed [10:0] draw_pipe_dy_center =
        11'sd240 - $signed({1'b0, draw_pipe_y});
    wire [23:0] draw_pipe_dx_sq = draw_pipe_dx_center * draw_pipe_dx_center;
    wire [23:0] draw_pipe_dy_sq = draw_pipe_dy_center * draw_pipe_dy_center;
    wire [24:0] draw_pipe_radius_sq = draw_pipe_dx_sq + draw_pipe_dy_sq;
    wire [40:0] draw_pipe_r2_prod = draw_pipe_radius_sq * fog_inv_proj_sq;
    wire [31:0] draw_pipe_r2_q16 = draw_pipe_r2_prod[31:0];
    wire [33:0] draw_pipe_ray_scale_q16 =
        34'd65536 + (({2'b00, draw_pipe_r2_q16} * 3'd3) >> 3);
    wire signed [11:0] draw_pipe_dx_center_o =
        $signed({1'b0, draw_pipe_x_o}) - 12'sd320;
    wire signed [10:0] draw_pipe_dy_center_o =
        11'sd240 - $signed({1'b0, draw_pipe_y_o});
    wire [23:0] draw_pipe_dx_sq_o = draw_pipe_dx_center_o * draw_pipe_dx_center_o;
    wire [23:0] draw_pipe_dy_sq_o = draw_pipe_dy_center_o * draw_pipe_dy_center_o;
    wire [24:0] draw_pipe_radius_sq_o = draw_pipe_dx_sq_o + draw_pipe_dy_sq_o;
    wire [40:0] draw_pipe_r2_prod_o = draw_pipe_radius_sq_o * fog_inv_proj_sq;
    wire [31:0] draw_pipe_r2_q16_o = draw_pipe_r2_prod_o[31:0];
    wire [33:0] draw_pipe_ray_scale_q16_o =
        34'd65536 + (({2'b00, draw_pipe_r2_q16_o} * 3'd3) >> 3);
    wire [65:0] fog0_radial_prod = fog0_w_q * fog0_ray_scale_q16;
    wire [31:0] fog0_radial_q16 = fog0_radial_prod[47:16];
    wire [15:0] fog0_radial_q8_8 = fog0_radial_q16[23:8];
    wire [65:0] fog0_radial_prod_o = fog0_w_q_o * fog0_ray_scale_q16_o;
    wire [31:0] fog0_radial_q16_o = fog0_radial_prod_o[47:16];
    wire [15:0] fog0_radial_q8_8_o = fog0_radial_q16_o[23:8];

    wire [15:0] fog_dist_span = fog_end_dist - fog_start_dist;
    wire [15:0] fog_dq1 = fog_start_dist + (fog_dist_span >> 2);
    wire [15:0] fog_dq2 = fog_start_dist + (fog_dist_span >> 1);
    wire [15:0] fog_dq3 = fog_end_dist - (fog_dist_span >> 2);
    wire fog1_fog_active = fog_enable &&
                           fog1_fog &&
                           (fog_end_dist > fog_start_dist) &&
                           (fog1_radial_q8_8 > fog_start_dist);
    wire fog1_fog_full = fog1_fog_active &&
                         (fog1_radial_q8_8 >= fog_end_dist);
    wire fog1_fog_active_o = fog_enable &&
                             fog1_fog_o &&
                             (fog_end_dist > fog_start_dist) &&
                             (fog1_radial_q8_8_o > fog_start_dist);
    wire fog1_fog_full_o = fog1_fog_active_o &&
                           (fog1_radial_q8_8_o >= fog_end_dist);
    // Map active fog depth into the 4-level blend_rgb565 alpha scale
    // (0=no fog, 1=25%, 2=50%, 3=75%). Pixels past fog_end_dist bypass the
    // blend entirely and take the fog color directly.
    wire [1:0] fog1_fog_alpha =
        !fog1_fog_active ? 2'd0 :
        fog1_fog_full    ? 2'd0 :
        (fog1_radial_q8_8 < fog_dq1) ? 2'd1 :
        (fog1_radial_q8_8 < fog_dq2) ? 2'd2 : 2'd3;
    wire [1:0] fog1_fog_alpha_o =
        !fog1_fog_active_o ? 2'd0 :
        fog1_fog_full_o    ? 2'd0 :
        (fog1_radial_q8_8_o < fog_dq1) ? 2'd1 :
        (fog1_radial_q8_8_o < fog_dq2) ? 2'd2 : 2'd3;
    wire [15:0] fog1_fog_blended =
        blend_rgb565(fog1_src_rgb565, fog1_fog_rgb565, fog1_fog_alpha);
    wire [15:0] fog1_fogged_rgb565 =
        fog1_fog_full ? fog1_fog_rgb565 : fog1_fog_blended;
    wire [15:0] fog1_out_rgb565 =
        blend_rgb565(fog1_fogged_rgb565, fog1_dst_rgb565, fog1_alpha);
    wire [15:0] fog1_fog_blended_o =
        blend_rgb565(fog1_src_rgb565_o, fog1_fog_rgb565_o, fog1_fog_alpha_o);
    wire [15:0] fog1_fogged_rgb565_o =
        fog1_fog_full_o ? fog1_fog_rgb565_o : fog1_fog_blended_o;
    wire [15:0] fog1_out_rgb565_o =
        blend_rgb565(fog1_fogged_rgb565_o, fog1_dst_rgb565_o, fog1_alpha_o);
    wire draw_commit_pass = draw_pipe_inside &&
                            !draw_pipe_transparent &&
                            (!draw_pipe_ztest || (draw_pipe_z < draw_pipe_z_ref));
    wire draw_commit_pass_o = draw_pipe_inside_o &&
                              !draw_pipe_transparent_o &&
                              (!draw_pipe_ztest_o || (draw_pipe_z_o < draw_pipe_z_ref_o));
    wire [31:0] status_word = {
        12'h0,
        {4'h0, fifo_count},
        vsy_latch,
        fifo_empty,
        fifo_full,
        engine_busy
    };

    wire [31:0] control_word = {
        28'h0,
        (clear_pending || (state == ST_CLEAR)),
        ctrl_ien,
        ctrl_flp_pending,
        ctrl_en
    };

    wire [23:0] pixel_rgb = {
        expand5_to_8(scan_rgb565_r[15:11]),
        expand6_to_8(scan_rgb565_r[10:5]),
        expand5_to_8(scan_rgb565_r[4:0])
    };

    function automatic [9:0] clamp_x(input logic signed [15:0] value);
        begin
            if (value < 0)
                clamp_x = 10'd0;
            else if (value > 16'sd639)
                clamp_x = 10'd639;
            else
                clamp_x = value[9:0];
        end
    endfunction

    function automatic [8:0] clamp_y(input logic signed [15:0] value);
        begin
            if (value < 0)
                clamp_y = 9'd0;
            else if (value > 16'sd479)
                clamp_y = 9'd479;
            else
                clamp_y = value[8:0];
        end
    endfunction

    function automatic [15:0] band_local_addr(input logic [9:0] x,
                                              input logic [8:0] y,
                                              input logic [2:0] band);
        logic [8:0] band_base_y;
        logic [8:0] local_y;
        begin
            band_base_y = band_base_row(band);
            local_y = (y >= band_base_y) ? (y - band_base_y) : 9'd0;
            band_local_addr = local_y * 16'd640 + {6'd0, x};
        end
    endfunction

    function automatic [15:0] band_pixel_count(input logic [2:0] band);
        logic [9:0]  base_row;
        logic [9:0]  band_end;
        logic [15:0] visible_rows;
        begin
            /* Use 10-bit row math: the final band is 448..479, and 448+64
             * wraps if this calculation is accidentally kept at 9 bits. */
            base_row = {1'b0, band_base_row(band)};
            band_end = base_row + 10'd64;
            if (base_row >= 10'd480)
                band_pixel_count = 16'd0;
            else if (band_end > 10'd480) begin
                visible_rows = {6'd0, (10'd480 - base_row)};
                band_pixel_count = visible_rows * 16'd640;
            end else begin
                band_pixel_count = 16'd40960;
            end
        end
    endfunction

    function automatic [24:0] band_word_count(input logic [2:0] band);
        begin
            band_word_count = {9'd0, band_pixel_count(band)};
        end
    endfunction

    function automatic [24:0] band_word_offset(input logic [2:0] band);
        begin
            /* band * 40960 = band*32768 + band*8192 = (band<<15) + (band<<13).
             * Encoding as shift+add avoids a synthesized variable-width
             * multiplier whose result mis-aliased on the Cyclone V build. */
            band_word_offset = {7'd0, band, 15'd0} + {9'd0, band, 13'd0};
        end
    endfunction

    function automatic [8:0] band_base_row(input logic [2:0] band);
        begin
            /* band * 64 == band << 6 */
            band_base_row = {band, 6'd0};
        end
    endfunction

    function automatic [7:0] sky_clear_start_palette(input logic [2:0] band);
        begin
            case (band)
                3'd0: sky_clear_start_palette = 8'd40;  // row 0
                3'd1: sky_clear_start_palette = 8'd43;  // row 64
                3'd2: sky_clear_start_palette = 8'd46;  // row 128
                3'd3: sky_clear_start_palette = 8'd49;  // row 192
                3'd4: sky_clear_start_palette = 8'd52;  // row 256
                3'd5: sky_clear_start_palette = 8'd56;  // row 320
                3'd6: sky_clear_start_palette = 8'd59;  // row 384
                default: sky_clear_start_palette = 8'd62; // row 448
            endcase
        end
    endfunction

    function automatic [4:0] sky_clear_start_row_count(input logic [2:0] band);
        begin
            case (band)
                3'd0,
                3'd5: sky_clear_start_row_count = 5'd0;
                3'd1,
                3'd6: sky_clear_start_row_count = 5'd4;
                3'd2,
                3'd7: sky_clear_start_row_count = 5'd8;
                3'd3: sky_clear_start_row_count = 5'd12;
                default: sky_clear_start_row_count = 5'd16;
            endcase
        end
    endfunction

    function automatic [2:0] y_to_band(input logic [8:0] y);
        begin
            /* y / 64 */
            y_to_band = y[8:6];
        end
    endfunction

    function automatic [15:0] clamp_z(input logic signed [47:0] value);
        begin
            if (value < 0)
                clamp_z = 16'h0000;
            else if (value > 48'sd65535)
                clamp_z = 16'hFFFF;
            else
                clamp_z = value[15:0];
        end
    endfunction

    function automatic signed [31:0] clamp_s32(input logic signed [63:0] value);
        begin
            if (value > 64'sh7FFF_FFFF)
                clamp_s32 = 32'sh7FFF_FFFF;
            else if (value < -64'sh8000_0000)
                clamp_s32 = -32'sh8000_0000;
            else
                clamp_s32 = value[31:0];
        end
    endfunction

    function automatic [31:0] clamp_pos_u32(input logic signed [63:0] value);
        begin
            if (value <= 0)
                clamp_pos_u32 = 32'd0;
            else if (value > 64'sh7FFF_FFFF)
                clamp_pos_u32 = 32'h7FFF_FFFF;
            else
                clamp_pos_u32 = value[31:0];
        end
    endfunction

    function automatic [5:0] msb_index32(input logic [31:0] value);
        integer bit_idx;
        logic found;
        begin
            msb_index32 = 6'd0;
            found = 1'b0;
            for (bit_idx = 31; bit_idx >= 0; bit_idx = bit_idx - 1) begin
                if (!found && value[bit_idx]) begin
                    msb_index32 = bit_idx[5:0];
                    found = 1'b1;
                end
            end
        end
    endfunction

    function automatic [3:0] texture_coord(input logic signed [63:0] value,
                                           input logic repeat_uv);
        begin
            if (repeat_uv)
                texture_coord = value[35:32];
            else if (value <= 64'sd0)
                texture_coord = 4'd0;
            else if (value >= 64'sh0000_0010_0000_0000)
                texture_coord = 4'd15;
            else
                texture_coord = value[35:32];
        end
    endfunction

    function automatic [15:0] rgb888_to_rgb565(input logic [23:0] rgb888);
        begin
            rgb888_to_rgb565 = {
                rgb888[23:19],
                rgb888[15:10],
                rgb888[7:3]
            };
        end
    endfunction

    function automatic [15:0] blend_rgb565(
        input logic [15:0] src,
        input logic [15:0] dst,
        input logic  [1:0] alpha
    );
        logic [6:0] r_mix;
        logic [7:0] g_mix;
        logic [6:0] b_mix;
        begin
            case (alpha)
                2'd1: begin  // 75% source, 25% destination
                    r_mix = ({2'd0, src[15:11]} * 3'd3) + {2'd0, dst[15:11]} + 7'd2;
                    g_mix = ({2'd0, src[10:5]} * 3'd3) + {2'd0, dst[10:5]} + 8'd2;
                    b_mix = ({2'd0, src[4:0]} * 3'd3) + {2'd0, dst[4:0]} + 7'd2;
                    blend_rgb565 = {r_mix[6:2], g_mix[7:2], b_mix[6:2]};
                end
                2'd2: begin  // 50% source, 50% destination
                    r_mix = {1'd0, src[15:11]} + {1'd0, dst[15:11]} + 7'd1;
                    g_mix = {1'd0, src[10:5]} + {1'd0, dst[10:5]} + 8'd1;
                    b_mix = {1'd0, src[4:0]} + {1'd0, dst[4:0]} + 7'd1;
                    blend_rgb565 = {r_mix[5:1], g_mix[6:1], b_mix[5:1]};
                end
                2'd3: begin  // 25% source, 75% destination
                    r_mix = {2'd0, src[15:11]} + ({2'd0, dst[15:11]} * 3'd3) + 7'd2;
                    g_mix = {2'd0, src[10:5]} + ({2'd0, dst[10:5]} * 3'd3) + 8'd2;
                    b_mix = {2'd0, src[4:0]} + ({2'd0, dst[4:0]} * 3'd3) + 7'd2;
                    blend_rgb565 = {r_mix[6:2], g_mix[7:2], b_mix[6:2]};
                end
                default: blend_rgb565 = src;
            endcase
        end
    endfunction

    function automatic [7:0] expand5_to_8(input logic [4:0] value);
        begin
            expand5_to_8 = {value, value[4:2]};
        end
    endfunction

    function automatic [7:0] expand6_to_8(input logic [5:0] value);
        begin
            expand6_to_8 = {value, value[5:4]};
        end
    endfunction

    function automatic [7:0] apply_light_bank(input logic [7:0] color,
                                              input logic [1:0] light_bank);
        begin
            if ((color == 8'd0) || (light_bank == 2'd0) || (color[7:6] != 2'b00))
                apply_light_bank = color;
            else
                apply_light_bank = {light_bank, color[5:0]};
        end
    endfunction

    voxel_vga_counters counters (
        .clk50       (clk),
        .reset       (reset),
        .hcount      (hcount),
        .vcount      (vcount),
        .VGA_CLK     (VGA_CLK),
        .VGA_HS      (VGA_HS),
        .VGA_VS      (VGA_VS),
        .VGA_BLANK_n (VGA_BLANK_n),
        .VGA_SYNC_n  (VGA_SYNC_n)
    );

    Sdram_Control sdram_ctrl (
        .REF_CLK     (clk),
        .RESET_N     (sdram_ctrl_reset_n),
        .CLK         (sdram_ctrl_clk),
        .WR_DATA     (cache_flush_state ? cache_word_pending : flush_word_pending),
        .WR          (sdram_wr_push),
        .WR_ADDR     (cache_flush_state ? sdram_wr_addr_cfg : flush_sdram_wr_addr),
        .WR_MAX_ADDR (cache_flush_state ? sdram_wr_max_addr_cfg : flush_sdram_wr_max_addr),
        .WR_LENGTH   (sdram_wr_length_cfg),
        .WR_LOAD     (cache_flush_state ? sdram_wr_load_pulse :
                      bg_flush_wr_load_req),
        .WR_CLK      (clk),
        .WR_FULL     (sdram_wr_full),
        .WR_USE      (sdram_wr_use),
        .RD_DATA     (sdram_rd_data),
        .RD          (sdram_rd_pop),
        .RD_ADDR     (sdram_rd_addr_cfg),
        .RD_MAX_ADDR (sdram_rd_max_addr_cfg),
        .RD_LENGTH   (sdram_rd_length_cfg),
        .RD_LOAD     (sdram_rd_load_out),
        .RD_CLK      (clk),
        .RD_EMPTY    (sdram_rd_empty),
        .RD_USE      (sdram_rd_use),
        .SA          (DRAM_ADDR),
        .BA          (DRAM_BA),
        .CS_N        (dram_cs_n_bus),
        .CKE         (DRAM_CKE),
        .RAS_N       (DRAM_RAS_N),
        .CAS_N       (DRAM_CAS_N),
        .WE_N        (DRAM_WE_N),
        .DQ          (DRAM_DQ),
        .DQM         ({DRAM_UDQM, DRAM_LDQM}),
        .SDR_CLK     (DRAM_CLK)
    );

    assign DRAM_CS_N = dram_cs_n_bus[0];

    /* ── Ping-pong band caches A and B ───────────────────── *
     * Banked by addr[0] (= x[0]) so even/odd x land in disjoint
     * SDP RAMs. The wrapper now exposes per-bank read and write
     * ports directly; the cache port driver below produces them by
     * fanning out the unified addr/data/en to both banks and
     * qualifying each bank's wr_en by linear addr[0]. The 1 px/cycle
     * rasterizer drives both banks of the active cache with the same
     * address; step 4b/c will start driving the lane-A and lane-B
     * ports independently for 2 px/cycle. */
    voxel_banked_sdp_ram #(
        .DATA_W(16),
        .ADDR_W(16),
        .DEPTH(BAND_PIXELS)
    ) fb_back_ram_A (
        .clk       (clk),
        .rd_addr_e (fb_A_e_rd_addr),
        .rd_data_e (fb_A_e_rd_data),
        .rd_addr_o (fb_A_o_rd_addr),
        .rd_data_o (fb_A_o_rd_data),
        .wr_addr_e (fb_A_e_wr_addr),
        .wr_data_e (fb_A_e_wr_data),
        .wr_en_e   (fb_A_e_wr_en),
        .wr_addr_o (fb_A_o_wr_addr),
        .wr_data_o (fb_A_o_wr_data),
        .wr_en_o   (fb_A_o_wr_en)
    );

    voxel_banked_sdp_ram #(
        .DATA_W(16),
        .ADDR_W(16),
        .DEPTH(BAND_PIXELS)
    ) fb_back_ram_B (
        .clk       (clk),
        .rd_addr_e (fb_B_e_rd_addr),
        .rd_data_e (fb_B_e_rd_data),
        .rd_addr_o (fb_B_o_rd_addr),
        .rd_data_o (fb_B_o_rd_data),
        .wr_addr_e (fb_B_e_wr_addr),
        .wr_data_e (fb_B_e_wr_data),
        .wr_en_e   (fb_B_e_wr_en),
        .wr_addr_o (fb_B_o_wr_addr),
        .wr_data_o (fb_B_o_wr_data),
        .wr_en_o   (fb_B_o_wr_en)
    );

    voxel_banked_sdp_ram #(
        .DATA_W(16),
        .ADDR_W(16),
        .DEPTH(BAND_PIXELS)
    ) z_ram_A (
        .clk       (clk),
        .rd_addr_e (z_A_e_rd_addr),
        .rd_data_e (z_A_e_rd_data),
        .rd_addr_o (z_A_o_rd_addr),
        .rd_data_o (z_A_o_rd_data),
        .wr_addr_e (z_A_e_wr_addr),
        .wr_data_e (z_A_e_wr_data),
        .wr_en_e   (z_A_e_wr_en),
        .wr_addr_o (z_A_o_wr_addr),
        .wr_data_o (z_A_o_wr_data),
        .wr_en_o   (z_A_o_wr_en)
    );

    voxel_banked_sdp_ram #(
        .DATA_W(16),
        .ADDR_W(16),
        .DEPTH(BAND_PIXELS)
    ) z_ram_B (
        .clk       (clk),
        .rd_addr_e (z_B_e_rd_addr),
        .rd_data_e (z_B_e_rd_data),
        .rd_addr_o (z_B_o_rd_addr),
        .rd_data_o (z_B_o_rd_data),
        .wr_addr_e (z_B_e_wr_addr),
        .wr_data_e (z_B_e_wr_data),
        .wr_en_e   (z_B_e_wr_en),
        .wr_addr_o (z_B_o_wr_addr),
        .wr_data_o (z_B_o_wr_data),
        .wr_en_o   (z_B_o_wr_en)
    );

    /*
     * Combine each cache's per-bank read outputs back into a unified
     * `rd_data` view. The 1-cycle-delayed LSB of the read address picks
     * whichever bank actually held the requested word — matches the
     * behavior of the old in-wrapper rd_sel_q mux exactly.
     */
    assign fb_A_rd_data = fb_A_rd_sel_q ? fb_A_o_rd_data : fb_A_e_rd_data;
    assign fb_B_rd_data = fb_B_rd_sel_q ? fb_B_o_rd_data : fb_B_e_rd_data;
    assign z_A_rd_data  = z_A_rd_sel_q  ? z_A_o_rd_data  : z_A_e_rd_data;
    assign z_B_rd_data  = z_B_rd_sel_q  ? z_B_o_rd_data  : z_B_e_rd_data;

    /*
     * Texture-atlas ROM. Sized to match TEXTURE_BYTES = 64 tiles *
     * 16 * 16 = 16384 entries (14-bit address). See voxel_texture_rom
     * for why we instantiate altsyncram directly instead of letting
     * Quartus infer a ramstyle="M10K" array -- the short version is
     * that inference can silently pick 2-cycle latency, producing
     * colored 1-pixel fringes at every quad boundary.
     */
    voxel_texture_rom #(
        .DATA_W(8),
        .ADDR_W(14),
        .DEPTH(TEXTURE_BYTES),
        /*
         * The texture atlas lives as a single `.mif` file, produced by
         * hw/voxel_gpu/scripts/generate_textures.py. Quartus consumes it here via the
         * altsyncram init_file, and the Python virtual hardware parses
         * the exact same file at runtime (see
         * virtualhw.raster.load_texture_mif) -- one source of truth.
         * Note: altsyncram does NOT accept Verilog $readmemh-style hex
         * files, only .mif or Intel-format .hex.
         */
        .INIT_FILE("voxel_gpu/assets/textures.mif")
    ) texture_rom (
        .clk       (clk),
        .rd_addr   (pipe2_tex_addr),
        .rd_data   (tex_rd_data),
        /*
         * Port B serves the odd lane of the 2 px/cycle draw pipe.
         */
        .rd_addr_b (pipe2_tex_addr_o),
        .rd_data_b (tex_rd_data_o)
    );

    integer i;
    integer ei;
    initial begin
        ctrl_en          = 1'b0;
        ctrl_ien         = 1'b0;
        ctrl_flp_pending = 1'b0;
        clear_pending    = 1'b0;
        frame_count      = 32'h0;
        pal_addr         = 8'h0;
        fog_enable       = 1'b0;
        fog_color        = 8'd0;
        fog_start_dist   = 16'd0;
        fog_end_dist     = 16'd0;
        fog_inv_proj_sq  = 16'd0;
        extmem_ctrl      = DEFAULT_EXTMEM_CTRL;
        extmem_front_base = DEFAULT_EXTMEM_FRONT_BASE;
        extmem_back_base = DEFAULT_EXTMEM_BACK_BASE;
        extmem_stride_bytes = DEFAULT_EXTMEM_STRIDE;
        extmem_tile_cfg  = 32'd0;
        extmem_dma_status = 32'd0;
        vsy_latch        = 1'b0;
        display_sel      = 1'b0;
        display_valid    = 1'b0;
        copy_target_sel  = 1'b1;
        copy_complete_pending = 1'b0;
        state            = ST_IDLE;
        fifo_wr_ptr      = 11'd0;
        fifo_rd_ptr      = 11'd0;
        fifo_count       = 12'd0;
        fetch_count      = 6'd0;
        draw_flush_count = 4'd0;
        clear_addr       = 16'd0;
        draw_row_base    = 16'd0;
        draw_x_min       = 10'd0;
        draw_x_max       = 10'd0;
        draw_x_cur       = 10'd0;
        draw_y_min       = 9'd0;
        draw_y_max       = 9'd0;
        draw_y_cur       = 9'd0;
        draw_tex_or_color = 8'd0;
        draw_flags       = 8'd0;
        draw_z0          = 16'd0;
        draw_dz_dx       = 16'sd0;
        draw_dz_dy       = 16'sd0;
        draw_uw_0        = 32'sd0;
        draw_uw_dx       = 32'sd0;
        draw_uw_dy       = 32'sd0;
        draw_vw_0        = 32'sd0;
        draw_vw_dx       = 32'sd0;
        draw_vw_dy       = 32'sd0;
        draw_iw_0        = 32'sd0;
        draw_iw_dx       = 32'sd0;
        draw_iw_dy       = 32'sd0;
        z_row_val        = 48'sd0;
        z_cur_val        = 48'sd0;
        uw_row_val       = 64'sd0;
        uw_cur_val       = 64'sd0;
        vw_row_val       = 64'sd0;
        vw_cur_val       = 64'sd0;
        iw_row_val       = 64'sd0;
        iw_cur_val       = 64'sd0;
        pipe0_valid      = 1'b0;
        pipe0_inside     = 1'b0;
        pipe0_ztest      = 1'b0;
        pipe0_textured   = 1'b0;
        pipe0_alpha_key  = 1'b0;
        pipe0_alpha      = 2'd0;
        pipe0_fog        = 1'b0;
        pipe0_light_bank = 2'd0;
        pipe0_tex_or_color = 8'd0;
        pipe0_addr       = 16'd0;
        pipe0_z          = 16'd0;
        pipe0_x          = 10'd0;
        pipe0_y          = 9'd0;
        pipe0_uw_q       = 32'sd0;
        pipe0_vw_q       = 32'sd0;
        pipe0_iw_q       = 32'd0;
        recip0_valid     = 1'b0;
        recip0_inside    = 1'b0;
        recip0_ztest     = 1'b0;
        recip0_textured  = 1'b0;
        recip0_alpha_key = 1'b0;
        recip0_alpha     = 2'd0;
        recip0_fog       = 1'b0;
        recip0_light_bank = 2'd0;
        recip0_tex_or_color = 8'd0;
        recip0_addr      = 16'd0;
        recip0_z         = 16'd0;
        recip0_x         = 10'd0;
        recip0_y         = 9'd0;
        recip0_uw_q      = 32'sd0;
        recip0_vw_q      = 32'sd0;
        recip0_iw_zero   = 1'b1;
        recip0_iw_msb    = 6'd0;
        recip0_iw_norm_q = 32'd0;
        recip1_valid     = 1'b0;
        recip1_inside    = 1'b0;
        recip1_ztest     = 1'b0;
        recip1_textured  = 1'b0;
        recip1_alpha_key = 1'b0;
        recip1_alpha     = 2'd0;
        recip1_fog       = 1'b0;
        recip1_light_bank = 2'd0;
        recip1_tex_or_color = 8'd0;
        recip1_addr      = 16'd0;
        recip1_z         = 16'd0;
        recip1_z_ref     = 16'd0;
        recip1_dst_rgb565 = 16'h0000;
        recip1_x         = 10'd0;
        recip1_y         = 9'd0;
        recip1_uw_q      = 32'sd0;
        recip1_vw_q      = 32'sd0;
        recip1_iw_zero   = 1'b1;
        recip1_iw_msb    = 6'd0;
        recip1_iw_lut_frac = 6'd0;
        recip1_w_norm_lo = 32'd0;
        recip1_w_norm_hi = 32'd0;
        recip2_valid     = 1'b0;
        recip2_inside    = 1'b0;
        recip2_ztest     = 1'b0;
        recip2_textured  = 1'b0;
        recip2_alpha_key = 1'b0;
        recip2_alpha     = 2'd0;
        recip2_fog       = 1'b0;
        recip2_light_bank = 2'd0;
        recip2_tex_or_color = 8'd0;
        recip2_addr      = 16'd0;
        recip2_z         = 16'd0;
        recip2_z_ref     = 16'd0;
        recip2_dst_rgb565 = 16'h0000;
        recip2_x         = 10'd0;
        recip2_y         = 9'd0;
        recip2_uw_q      = 32'sd0;
        recip2_vw_q      = 32'sd0;
        recip2_iw_zero   = 1'b1;
        recip2_iw_msb    = 6'd0;
        recip2_w_norm_q  = 32'd0;
        pipe1_valid      = 1'b0;
        pipe1_inside     = 1'b0;
        pipe1_ztest      = 1'b0;
        pipe1_textured   = 1'b0;
        pipe1_alpha_key  = 1'b0;
        pipe1_alpha      = 2'd0;
        pipe1_fog        = 1'b0;
        pipe1_light_bank = 2'd0;
        pipe1_tex_or_color = 8'd0;
        pipe1_addr       = 16'd0;
        pipe1_z          = 16'd0;
        pipe1_z_ref      = 16'd0;
        pipe1_dst_rgb565 = 16'h0000;
        pipe1_x          = 10'd0;
        pipe1_y          = 9'd0;
        pipe1_uw_q       = 32'sd0;
        pipe1_vw_q       = 32'sd0;
        pipe1_w_q        = 32'd0;
        tex0_valid       = 1'b0;
        tex0_inside      = 1'b0;
        tex0_ztest       = 1'b0;
        tex0_textured    = 1'b0;
        tex0_alpha_key   = 1'b0;
        tex0_alpha       = 2'd0;
        tex0_fog         = 1'b0;
        tex0_light_bank  = 2'd0;
        tex0_tex_or_color = 8'd0;
        tex0_addr        = 16'd0;
        tex0_z           = 16'd0;
        tex0_z_ref       = 16'd0;
        tex0_dst_rgb565  = 16'h0000;
        tex0_x           = 10'd0;
        tex0_y           = 9'd0;
        tex0_w_q         = 32'd0;
        tex0_u_prod      = 64'sd0;
        tex0_v_prod      = 64'sd0;
        pipe2_valid      = 1'b0;
        pipe2_inside     = 1'b0;
        pipe2_ztest      = 1'b0;
        pipe2_textured   = 1'b0;
        pipe2_alpha_key  = 1'b0;
        pipe2_alpha      = 2'd0;
        pipe2_fog        = 1'b0;
        pipe2_light_bank = 2'd0;
        pipe2_tex_or_color = 8'd0;
        pipe2_addr       = 16'd0;
        pipe2_z          = 16'd0;
        pipe2_z_ref      = 16'd0;
        pipe2_dst_rgb565 = 16'h0000;
        pipe2_x          = 10'd0;
        pipe2_y          = 9'd0;
        pipe2_w_q        = 32'd0;
        pipe2_tex_addr   = 14'd0;
        draw_pipe_valid  = 1'b0;
        draw_pipe_inside = 1'b0;
        draw_pipe_ztest  = 1'b0;
        draw_pipe_textured = 1'b0;
        draw_pipe_alpha_key = 1'b0;
        draw_pipe_alpha = 2'd0;
        draw_pipe_fog    = 1'b0;
        draw_pipe_light_bank = 2'd0;
        draw_pipe_tex_or_color = 8'd0;
        draw_pipe_addr   = 16'd0;
        draw_pipe_z      = 16'd0;
        draw_pipe_z_ref  = 16'd0;
        draw_pipe_dst_rgb565 = 16'h0000;
        draw_pipe_x      = 10'd0;
        draw_pipe_y      = 9'd0;
        draw_pipe_w_q    = 32'd0;
        draw_is_band_primer = 1'b0;
        pal_rd_valid     = 1'b0;
        pal_rd_pass      = 1'b0;
        pal_rd_ztest     = 1'b0;
        pal_rd_alpha     = 2'd0;
        pal_rd_fog       = 1'b0;
        pal_rd_addr      = 16'd0;
        pal_rd_z         = 16'd0;
        pal_rd_src_addr  = 8'd0;
        pal_rd_fog_addr  = 8'd0;
        pal_rd_dst_rgb565 = 16'h0000;
        pal_rd_w_q       = 32'd0;
        pal_rd_ray_scale_q16 = 34'd0;
        plr_valid        = 1'b0;
        plr_pass         = 1'b0;
        plr_ztest        = 1'b0;
        plr_alpha        = 2'd0;
        plr_fog          = 1'b0;
        plr_addr         = 16'd0;
        plr_z            = 16'd0;
        plr_src_rgb      = 24'h000000;
        plr_dst_rgb565   = 16'h0000;
        plr_fog_rgb      = 24'h000000;
        plr_w_q          = 32'd0;
        plr_ray_scale_q16 = 34'd0;
        fog0_valid       = 1'b0;
        fog0_pass        = 1'b0;
        fog0_ztest       = 1'b0;
        fog0_alpha       = 2'd0;
        fog0_fog         = 1'b0;
        fog0_addr        = 16'd0;
        fog0_z           = 16'd0;
        fog0_src_rgb565  = 16'h0000;
        fog0_dst_rgb565  = 16'h0000;
        fog0_fog_rgb565  = 16'h0000;
        fog0_w_q         = 32'd0;
        fog0_ray_scale_q16 = 34'd0;
        fog1_valid       = 1'b0;
        fog1_pass        = 1'b0;
        fog1_ztest       = 1'b0;
        fog1_alpha       = 2'd0;
        fog1_fog         = 1'b0;
        fog1_addr        = 16'd0;
        fog1_z           = 16'd0;
        fog1_src_rgb565  = 16'h0000;
        fog1_dst_rgb565  = 16'h0000;
        fog1_fog_rgb565  = 16'h0000;
        fog1_radial_q8_8 = 16'd0;
        commit_valid     = 1'b0;
        commit_pass      = 1'b0;
        commit_ztest     = 1'b0;
        commit_addr      = 16'd0;
        commit_z         = 16'd0;
        commit_color     = 16'h0000;
        pipe0_valid_o    = 1'b0;
        recip0_valid_o   = 1'b0;
        recip1_valid_o   = 1'b0;
        recip2_valid_o   = 1'b0;
        pipe1_valid_o    = 1'b0;
        tex0_valid_o     = 1'b0;
        pipe2_valid_o    = 1'b0;
        draw_pipe_valid_o = 1'b0;
        pal_rd_valid_o   = 1'b0;
        plr_valid_o      = 1'b0;
        fog0_valid_o     = 1'b0;
        fog1_valid_o     = 1'b0;
        commit_valid_o   = 1'b0;
        scan_rgb565_r    = 16'h0000;
        scan_visible_r   = 1'b0;
        scan_line0_ready = 1'b0;
        scan_line1_ready = 1'b0;
        scan_line2_ready = 1'b0;
        scan_line0_row   = 9'd0;
        scan_line1_row   = 9'd0;
        scan_line2_row   = 9'd0;
        scan_active_bank = 2'd0;
        scan_active_valid = 1'b0;
        scan_active_row  = 9'd0;
        scan_fill_active = 1'b0;
        scan_fill_armed  = 1'b0;
        scan_fill_load_pending = 1'b0;
        scan_fill_bank   = 2'd0;
        scan_fill_row    = 9'd0;
        scan_fill_base_words = 25'd0;
        scan_fill_store_idx = 10'd0;
        scan_rd_capture = 1'b0;
        sdram_rd_addr_cfg = 25'd0;
        sdram_rd_max_addr_cfg = 25'd0;
        sdram_rd_load_pulse = 1'b0;
        sdram_rd_load_stretch_req = 1'b0;
        sdram_rd_load_hold  = 4'd0;
        cache_band_index = 3'd0;
        cache_target_band = 3'd0;
        cache_valid = 1'b0;
        cache_dirty = 1'b0;
        cache_draw_dirty = 1'b0;
        cache_band_valid = 8'h00;
        cache_resume_draw = 1'b0;
        cache_final_flush = 1'b0;
        cache_maint_addr = 16'd0;
        cache_pixels_total = 16'd0;
        cache_init_x = 10'd0;
        cache_init_sky_row_count = 5'd0;
        cache_init_sky_palette = PAL_SKY_GRADIENT_BASE;
        cache_words_issued = 16'd0;
        cache_words_done = 16'd0;
        cache_fetch_inflight = 1'b0;
        cache_drain_count = 8'd0;
        cache_word_pending_valid = 1'b0;
        cache_word_pending = 16'd0;
        cache_load_is_z = 1'b0;
        cache_rd_capture = 1'b0;
        sdram_wr_addr_cfg = 25'd0;
        sdram_wr_max_addr_cfg = 25'd0;
        sdram_wr_load_pulse = 1'b0;
        flush_active = 1'b0;
        flush_maint_addr = 16'd0;
        flush_pixels_total = 16'd0;
        flush_words_issued = 16'd0;
        flush_words_done = 16'd0;
        flush_fetch_inflight = 1'b0;
        flush_word_pending_valid = 1'b0;
        flush_word_pending = 16'd0;
        flush_load_pending = 1'b0;
        flush_drain_count = 8'd0;
        flush_band_index = 3'd0;
        flush_sdram_wr_addr = 25'd0;
        flush_sdram_wr_max_addr = 25'd0;
        flush_cache_sel = 1'b0;
        flush_generated_sky = 1'b0;
        flush_sky_x = 10'd0;
        flush_sky_row_count = 5'd0;
        flush_sky_palette = PAL_SKY_GRADIENT_BASE;
        sdram_powerup_counter = 18'd0;
        sdram_init_wait_counter = 16'd0;
        sdram_ctrl_reset_n = 1'b0;
        sdram_ready = 1'b0;
        vga_vs_d         = 1'b1;

        palette[0]  = 24'h101018;
        palette[1]  = 24'h6BA43A;
        palette[2]  = 24'h8B6341;
        palette[3]  = 24'h6F5737;
        palette[4]  = 24'h7C7C7C;
        palette[5]  = 24'hFFFFFF;
        palette[6]  = 24'hFF4040;
        palette[7]  = 24'h40A0FF;
        palette[8]  = 24'hFFD040;
        palette[9]  = 24'h5C8634;
        palette[10] = 24'h6A4A2C;
        palette[11] = 24'h9D7B4D;
        palette[12] = 24'h533823;
        palette[13] = 24'h989898;
        palette[14] = 24'h5C5C5C;
        palette[15] = 24'hA77952;
        palette[16] = 24'h59412A;
        palette[17] = 24'h4F782D;
        palette[18] = 24'h84BA57;
        palette[19] = 24'h6F4F32;
        palette[20] = 24'hAA815A;
        palette[21] = 24'h886A44;
        palette[22] = 24'h503B24;
        palette[23] = 24'h636363;
        palette[24] = 24'h9A9A9A;
        for (i = 25; i < 256; i = i + 1)
            palette[i] = {i[7:0], i[7:0], i[7:0]};

        for (i = 0; i < FIFO_DEPTH; i = i + 1)
            fifo_mem[i] = 32'h0;

        /*
         * The texture atlas is loaded by voxel_texture_rom via
         * altsyncram init_file (voxel_gpu/assets/textures.mif); no $readmemh needed
         * for it here anymore.
         */
        $readmemh("voxel_gpu/assets/recip_lut.hex", recip_lut);

        for (i = 0; i < MAX_DESC_WORDS; i = i + 1)
            desc_words[i] = 32'h0;

        for (i = 0; i < 4; i = i + 1) begin
            edge_A[i] = 32'sd0;
            edge_B[i] = 32'sd0;
            edge_C[i] = 32'sd0;
            edge_row_val[i] = 64'sd0;
            edge_cur_val[i] = 64'sd0;
        end

        for (i = 0; i < LINE_WORDS; i = i + 1) begin
            scan_linebuf0[i] = 16'h0000;
            scan_linebuf1[i] = 16'h0000;
            scan_linebuf2[i] = 16'h0000;
        end
    end

    always_comb begin
        if (!scan_current_x_valid)
            scan_rgb565_now = 16'h0000;
        else if (scan_active_bank == 2'd1)
            scan_rgb565_now = scan_linebuf1[scan_current_x];
        else if (scan_active_bank == 2'd2)
            scan_rgb565_now = scan_linebuf2[scan_current_x];
        else
            scan_rgb565_now = scan_linebuf0[scan_current_x];

        // VGA_BLANK_n is registered inside voxel_vga_counters (1-cycle late vs
        // hcount). scan_visible_r is then registered again here, putting it
        // 2 cycles behind hcount. scan_rgb565_r is only 1 cycle behind. The
        // resulting phase mismatch blanks pixel 0 every line (linebuf[0] is
        // computed but the visibility flag is still showing last cycle's
        // blanking from hcount=1599). Use the combinational visible predicate
        // so scan_visible_r and VGA_BLANK_n at the DAC are in phase.
        scan_visible_now = scan_current_x_valid && vcount_visible &&
                           scan_visible_data_ready;
        if (!scan_visible_now)
            scan_rgb565_now = 16'h0000;

        fb_back_rd_addr = cache_flush_state ? cache_maint_addr : pipe0_addr;
        fb_back_rd_addr_o = cache_flush_state ? cache_maint_addr : pipe0_addr_o;
        draw_addr = draw_cache_addr;
        draw_addr_o = draw_cache_addr_o;
        fb_wr_addr = draw_addr;
        fb_wr_data = 16'h0000;
        fb_back_wr_en = 1'b0;
        fb_wr_addr_e = fb_wr_addr;
        fb_wr_data_e = fb_wr_data;
        fb_back_wr_en_e = fb_back_wr_en && (fb_wr_addr[0] == 1'b0);
        fb_wr_addr_o = fb_wr_addr;
        fb_wr_data_o = fb_wr_data;
        fb_back_wr_en_o = fb_back_wr_en && (fb_wr_addr[0] == 1'b1);
        z_rd_addr = cache_flush_state ? cache_maint_addr : pipe0_addr;
        z_rd_addr_o = cache_flush_state ? cache_maint_addr : pipe0_addr_o;
        z_wr_addr = draw_pipe_addr;
        z_wr_data = draw_pipe_z;
        z_wr_en   = 1'b0;
        z_wr_addr_e = z_wr_addr;
        z_wr_data_e = z_wr_data;
        z_wr_en_e = z_wr_en && (z_wr_addr[0] == 1'b0);
        z_wr_addr_o = z_wr_addr;
        z_wr_data_o = z_wr_data;
        z_wr_en_o = z_wr_en && (z_wr_addr[0] == 1'b1);

        case (state)
            ST_CLEAR: begin
                fb_wr_addr = clear_addr;
                fb_wr_data = clear_rgb565;
                z_wr_addr = clear_addr;
                z_wr_data = 16'hFFFF;
                z_wr_en   = 1'b1;
                fb_back_wr_en = 1'b1;
            end

            ST_FETCH: begin
                if (cache_sky_patch_state) begin
                    fb_wr_addr = band_local_addr(desc_x_min, desc_y_min, cache_band_index);
                    fb_wr_data = rgb888_to_rgb565(palette[desc_tex_or_color]);
                    fb_back_wr_en = 1'b1;
                end
            end

            ST_CACHE_INIT: begin
                /* 1 px/cycle init. The 2 px/cycle variant cost 43 LABs we
                 * don't have (PROJECT_NOTES May 5 fitter result). The
                 * cache reuse fix (gpu_transport.c BACKBUF_EN drop) makes
                 * most cached bands skip ST_CACHE_INIT entirely, so the
                 * one-pixel init runs only on uncached bands. */
                fb_wr_addr = cache_maint_addr;
                fb_wr_data = cache_init_rgb565;
                fb_back_wr_en = 1'b1;
                z_wr_addr = cache_maint_addr;
                z_wr_data = 16'hFFFF;
                z_wr_en = 1'b1;
            end

            ST_CACHE_LOAD_COLOR: begin
                if (cache_rd_capture) begin
                    fb_wr_addr = cache_maint_addr;
                    fb_wr_data = sdram_rd_data;
                    fb_back_wr_en = 1'b1;
                end
            end

            ST_CACHE_LOAD_Z: begin
                if (cache_rd_capture) begin
                    z_wr_addr = cache_maint_addr;
                    z_wr_data = sdram_rd_data;
                    z_wr_en = 1'b1;
                end
            end

            ST_DRAW,
            ST_DRAW_FLUSH: begin
                if (commit_valid && commit_pass) begin
                    fb_wr_addr_e = commit_addr;
                    fb_wr_data_e = commit_color;
                    fb_back_wr_en_e = 1'b1;
                end

                if (commit_valid && commit_pass && commit_ztest) begin
                    z_wr_en_e = 1'b1;
                    z_wr_addr_e = commit_addr;
                    z_wr_data_e = commit_z;
                end

                if (commit_valid_o && commit_pass_o) begin
                    fb_wr_addr_o = commit_addr_o;
                    fb_wr_data_o = commit_color_o;
                    fb_back_wr_en_o = 1'b1;
                end

                if (commit_valid_o && commit_pass_o && commit_ztest_o) begin
                    z_wr_en_o = 1'b1;
                    z_wr_addr_o = commit_addr_o;
                    z_wr_data_o = commit_z_o;
                end
            end

            default: ;
        endcase

        /* ST_DRAW / ST_DRAW_FLUSH already drive the _e/_o write ports directly
         * inside the case above (lines 2426-2451). Every other state — including
         * ST_CACHE_INIT — only drives the unsuffixed fb_wr_addr / z_wr_en /
         * cache_maint_addr signals and relies on this block to fan them out to
         * the banked BRAM ports. Excluding ST_CACHE_INIT here previously left
         * its writes stranded on dead defaults: the Z-buffer never reset to
         * 0xFFFF (so depth-tested terrain failed the test against 0x0000) and
         * the color BRAM kept stale band content across frames (visible as
         * vertically-replicated HUD/sky strips). */
        if (!(state == ST_DRAW || state == ST_DRAW_FLUSH)) begin
            fb_wr_addr_e = fb_wr_addr;
            fb_wr_data_e = fb_wr_data;
            fb_back_wr_en_e = fb_back_wr_en && (fb_wr_addr[0] == 1'b0);
            fb_wr_addr_o = fb_wr_addr;
            fb_wr_data_o = fb_wr_data;
            fb_back_wr_en_o = fb_back_wr_en && (fb_wr_addr[0] == 1'b1);
            z_wr_addr_e = z_wr_addr;
            z_wr_data_e = z_wr_data;
            z_wr_en_e = z_wr_en && (z_wr_addr[0] == 1'b0);
            z_wr_addr_o = z_wr_addr;
            z_wr_data_o = z_wr_data;
            z_wr_en_o = z_wr_en && (z_wr_addr[0] == 1'b1);
        end

        /* ── Ping-pong port routing ────────────────────────────────────── */
        /* Rasterizer (draw_cache_sel): owns read+write of active cache.   */
        /* Flush controller (flush_cache_sel): reads from its cache.       */

        /* ── Cache A ports ── */
        if (draw_cache_sel == 1'b0 && cache_used_by_main) begin
            /* A is active: rasterizer read+write. */
            fb_A_e_rd_addr = fb_back_rd_addr;
            fb_A_o_rd_addr = fb_back_rd_addr_o;
            fb_A_e_wr_addr = fb_wr_addr_e;
            fb_A_o_wr_addr = fb_wr_addr_o;
            fb_A_e_wr_data = fb_wr_data_e;
            fb_A_o_wr_data = fb_wr_data_o;
            fb_A_e_wr_en   = fb_back_wr_en_e;
            fb_A_o_wr_en   = fb_back_wr_en_o;
            z_A_e_rd_addr  = z_rd_addr;
            z_A_o_rd_addr  = z_rd_addr_o;
            z_A_e_wr_addr  = z_wr_addr_e;
            z_A_o_wr_addr  = z_wr_addr_o;
            z_A_e_wr_data  = z_wr_data_e;
            z_A_o_wr_data  = z_wr_data_o;
            z_A_e_wr_en    = z_wr_en_e;
            z_A_o_wr_en    = z_wr_en_o;
        end else if (flush_cache_sel == 1'b0 && flush_active) begin
            /* A is being flushed: flush reads. */
            fb_A_e_rd_addr = flush_maint_addr;
            fb_A_o_rd_addr = flush_maint_addr;
            fb_A_e_wr_addr = 16'd0;
            fb_A_o_wr_addr = 16'd0;
            fb_A_e_wr_data = 16'd0;
            fb_A_o_wr_data = 16'd0;
            fb_A_e_wr_en   = 1'b0;
            fb_A_o_wr_en   = 1'b0;
            z_A_e_rd_addr  = flush_maint_addr;
            z_A_o_rd_addr  = flush_maint_addr;
            z_A_e_wr_addr  = 16'd0;
            z_A_o_wr_addr  = 16'd0;
            z_A_e_wr_data  = 16'd0;
            z_A_o_wr_data  = 16'd0;
            z_A_e_wr_en    = 1'b0;
            z_A_o_wr_en    = 1'b0;
        end else begin
            /* A is idle */
            fb_A_e_rd_addr = 16'd0;
            fb_A_o_rd_addr = 16'd0;
            fb_A_e_wr_addr = 16'd0;
            fb_A_o_wr_addr = 16'd0;
            fb_A_e_wr_data = 16'd0;
            fb_A_o_wr_data = 16'd0;
            fb_A_e_wr_en   = 1'b0;
            fb_A_o_wr_en   = 1'b0;
            z_A_e_rd_addr  = 16'd0;
            z_A_o_rd_addr  = 16'd0;
            z_A_e_wr_addr  = 16'd0;
            z_A_o_wr_addr  = 16'd0;
            z_A_e_wr_data  = 16'd0;
            z_A_o_wr_data  = 16'd0;
            z_A_e_wr_en    = 1'b0;
            z_A_o_wr_en    = 1'b0;
        end

        /* ── Cache B ports ── */
        if (draw_cache_sel == 1'b1 && cache_used_by_main) begin
            /* B is active: rasterizer read+write. */
            fb_B_e_rd_addr = fb_back_rd_addr;
            fb_B_o_rd_addr = fb_back_rd_addr_o;
            fb_B_e_wr_addr = fb_wr_addr_e;
            fb_B_o_wr_addr = fb_wr_addr_o;
            fb_B_e_wr_data = fb_wr_data_e;
            fb_B_o_wr_data = fb_wr_data_o;
            fb_B_e_wr_en   = fb_back_wr_en_e;
            fb_B_o_wr_en   = fb_back_wr_en_o;
            z_B_e_rd_addr  = z_rd_addr;
            z_B_o_rd_addr  = z_rd_addr_o;
            z_B_e_wr_addr  = z_wr_addr_e;
            z_B_o_wr_addr  = z_wr_addr_o;
            z_B_e_wr_data  = z_wr_data_e;
            z_B_o_wr_data  = z_wr_data_o;
            z_B_e_wr_en    = z_wr_en_e;
            z_B_o_wr_en    = z_wr_en_o;
        end else if (flush_cache_sel == 1'b1 && flush_active) begin
            /* B is being flushed: flush reads. */
            fb_B_e_rd_addr = flush_maint_addr;
            fb_B_o_rd_addr = flush_maint_addr;
            fb_B_e_wr_addr = 16'd0;
            fb_B_o_wr_addr = 16'd0;
            fb_B_e_wr_data = 16'd0;
            fb_B_o_wr_data = 16'd0;
            fb_B_e_wr_en   = 1'b0;
            fb_B_o_wr_en   = 1'b0;
            z_B_e_rd_addr  = flush_maint_addr;
            z_B_o_rd_addr  = flush_maint_addr;
            z_B_e_wr_addr  = 16'd0;
            z_B_o_wr_addr  = 16'd0;
            z_B_e_wr_data  = 16'd0;
            z_B_o_wr_data  = 16'd0;
            z_B_e_wr_en    = 1'b0;
            z_B_o_wr_en    = 1'b0;
        end else begin
            /* B is idle */
            fb_B_e_rd_addr = 16'd0;
            fb_B_o_rd_addr = 16'd0;
            fb_B_e_wr_addr = 16'd0;
            fb_B_o_wr_addr = 16'd0;
            fb_B_e_wr_data = 16'd0;
            fb_B_o_wr_data = 16'd0;
            fb_B_e_wr_en   = 1'b0;
            fb_B_o_wr_en   = 1'b0;
            z_B_e_rd_addr  = 16'd0;
            z_B_o_rd_addr  = 16'd0;
            z_B_e_wr_addr  = 16'd0;
            z_B_o_wr_addr  = 16'd0;
            z_B_e_wr_data  = 16'd0;
            z_B_o_wr_data  = 16'd0;
            z_B_e_wr_en    = 1'b0;
            z_B_o_wr_en    = 1'b0;
        end
    end

    always_ff @(posedge clk) begin
        if (reset) begin
            ctrl_en          <= 1'b0;
            ctrl_ien         <= 1'b0;
            ctrl_flp_pending <= 1'b0;
            clear_pending    <= 1'b0;
            frame_count      <= 32'h0;
            pal_addr         <= 8'h0;
            fog_enable       <= 1'b0;
            fog_color        <= 8'd0;
            fog_start_dist   <= 16'd0;
            fog_end_dist     <= 16'd0;
            fog_inv_proj_sq  <= 16'd0;
            extmem_ctrl      <= DEFAULT_EXTMEM_CTRL;
            extmem_front_base <= DEFAULT_EXTMEM_FRONT_BASE;
            extmem_back_base <= DEFAULT_EXTMEM_BACK_BASE;
            extmem_stride_bytes <= DEFAULT_EXTMEM_STRIDE;
            extmem_tile_cfg  <= 32'd0;
            extmem_dma_status <= 32'd0;
            vsy_latch        <= 1'b0;
            display_sel      <= 1'b0;
            display_valid    <= 1'b0;
            copy_target_sel  <= 1'b1;
            copy_complete_pending <= 1'b0;
            state            <= ST_IDLE;
            fifo_wr_ptr      <= 11'd0;
            fifo_rd_ptr      <= 11'd0;
            fifo_count       <= 12'd0;
            fetch_count      <= 6'd0;
            draw_flush_count <= 4'd0;
            clear_addr       <= 16'd0;
            draw_row_base    <= 16'd0;
            draw_x_min       <= 10'd0;
            draw_x_max       <= 10'd0;
            draw_x_cur       <= 10'd0;
            draw_y_min       <= 9'd0;
            draw_y_max       <= 9'd0;
            draw_y_cur       <= 9'd0;
            draw_tex_or_color <= 8'd0;
            draw_flags       <= 8'd0;
            draw_z0          <= 16'd0;
            draw_dz_dx       <= 16'sd0;
            draw_dz_dy       <= 16'sd0;
            draw_uw_0        <= 32'sd0;
            draw_uw_dx       <= 32'sd0;
            draw_uw_dy       <= 32'sd0;
            draw_vw_0        <= 32'sd0;
            draw_vw_dx       <= 32'sd0;
            draw_vw_dy       <= 32'sd0;
            draw_iw_0        <= 32'sd0;
            draw_iw_dx       <= 32'sd0;
            draw_iw_dy       <= 32'sd0;
            z_row_val        <= 48'sd0;
            z_cur_val        <= 48'sd0;
            uw_row_val       <= 64'sd0;
            uw_cur_val       <= 64'sd0;
            vw_row_val       <= 64'sd0;
            vw_cur_val       <= 64'sd0;
            iw_row_val       <= 64'sd0;
            iw_cur_val       <= 64'sd0;
            pipe0_valid      <= 1'b0;
            pipe0_inside     <= 1'b0;
            pipe0_ztest      <= 1'b0;
            pipe0_textured   <= 1'b0;
            pipe0_alpha_key  <= 1'b0;
            pipe0_alpha      <= 2'd0;
            pipe0_fog        <= 1'b0;
            pipe0_light_bank <= 2'd0;
            pipe0_tex_or_color <= 8'd0;
            pipe0_addr       <= 16'd0;
            pipe0_z          <= 16'd0;
            pipe0_x          <= 10'd0;
            pipe0_y          <= 9'd0;
            pipe0_uw_q       <= 32'sd0;
            pipe0_vw_q       <= 32'sd0;
            pipe0_iw_q       <= 32'd0;
            recip0_valid     <= 1'b0;
            recip0_inside    <= 1'b0;
            recip0_ztest     <= 1'b0;
            recip0_textured  <= 1'b0;
            recip0_alpha_key <= 1'b0;
            recip0_alpha     <= 2'd0;
            recip0_fog       <= 1'b0;
            recip0_light_bank <= 2'd0;
            recip0_tex_or_color <= 8'd0;
            recip0_addr      <= 16'd0;
            recip0_z         <= 16'd0;
            recip0_x         <= 10'd0;
            recip0_y         <= 9'd0;
            recip0_uw_q      <= 32'sd0;
            recip0_vw_q      <= 32'sd0;
            recip0_iw_zero   <= 1'b1;
            recip0_iw_msb    <= 6'd0;
            recip0_iw_norm_q <= 32'd0;
            recip1_valid     <= 1'b0;
            recip1_inside    <= 1'b0;
            recip1_ztest     <= 1'b0;
            recip1_textured  <= 1'b0;
            recip1_alpha_key <= 1'b0;
            recip1_alpha     <= 2'd0;
            recip1_fog       <= 1'b0;
            recip1_light_bank <= 2'd0;
            recip1_tex_or_color <= 8'd0;
            recip1_addr      <= 16'd0;
            recip1_z         <= 16'd0;
            recip1_z_ref     <= 16'd0;
            recip1_dst_rgb565 <= 16'h0000;
            recip1_x         <= 10'd0;
            recip1_y         <= 9'd0;
            recip1_uw_q      <= 32'sd0;
            recip1_vw_q      <= 32'sd0;
            recip1_iw_zero   <= 1'b1;
            recip1_iw_msb    <= 6'd0;
            recip1_iw_lut_frac <= 6'd0;
            recip1_w_norm_lo <= 32'd0;
            recip1_w_norm_hi <= 32'd0;
            recip2_valid     <= 1'b0;
            recip2_inside    <= 1'b0;
            recip2_ztest     <= 1'b0;
            recip2_textured  <= 1'b0;
            recip2_alpha_key <= 1'b0;
            recip2_alpha     <= 2'd0;
            recip2_fog       <= 1'b0;
            recip2_light_bank <= 2'd0;
            recip2_tex_or_color <= 8'd0;
            recip2_addr      <= 16'd0;
            recip2_z         <= 16'd0;
            recip2_z_ref     <= 16'd0;
            recip2_dst_rgb565 <= 16'h0000;
            recip2_x         <= 10'd0;
            recip2_y         <= 9'd0;
            recip2_uw_q      <= 32'sd0;
            recip2_vw_q      <= 32'sd0;
            recip2_iw_zero   <= 1'b1;
            recip2_iw_msb    <= 6'd0;
            recip2_w_norm_q  <= 32'd0;
            pipe1_valid      <= 1'b0;
            pipe1_inside     <= 1'b0;
            pipe1_ztest      <= 1'b0;
            pipe1_textured   <= 1'b0;
            pipe1_alpha_key  <= 1'b0;
            pipe1_alpha      <= 2'd0;
            pipe1_fog        <= 1'b0;
            pipe1_light_bank <= 2'd0;
            pipe1_tex_or_color <= 8'd0;
            pipe1_addr       <= 16'd0;
            pipe1_z          <= 16'd0;
            pipe1_z_ref      <= 16'd0;
            pipe1_dst_rgb565 <= 16'h0000;
            pipe1_x          <= 10'd0;
            pipe1_y          <= 9'd0;
            pipe1_uw_q       <= 32'sd0;
            pipe1_vw_q       <= 32'sd0;
            pipe1_w_q        <= 32'd0;
            tex0_valid       <= 1'b0;
            tex0_inside      <= 1'b0;
            tex0_ztest       <= 1'b0;
            tex0_textured    <= 1'b0;
            tex0_alpha_key   <= 1'b0;
            tex0_alpha       <= 2'd0;
            tex0_fog         <= 1'b0;
            tex0_light_bank  <= 2'd0;
            tex0_tex_or_color <= 8'd0;
            tex0_addr        <= 16'd0;
            tex0_z           <= 16'd0;
            tex0_z_ref       <= 16'd0;
            tex0_dst_rgb565  <= 16'h0000;
            tex0_x           <= 10'd0;
            tex0_y           <= 9'd0;
            tex0_w_q         <= 32'd0;
            tex0_u_prod      <= 64'sd0;
            tex0_v_prod      <= 64'sd0;
            pipe2_valid      <= 1'b0;
            pipe2_inside     <= 1'b0;
            pipe2_ztest      <= 1'b0;
            pipe2_textured   <= 1'b0;
            pipe2_alpha_key  <= 1'b0;
            pipe2_alpha      <= 2'd0;
            pipe2_fog        <= 1'b0;
            pipe2_light_bank <= 2'd0;
            pipe2_tex_or_color <= 8'd0;
            pipe2_addr       <= 16'd0;
            pipe2_z          <= 16'd0;
            pipe2_z_ref      <= 16'd0;
            pipe2_dst_rgb565 <= 16'h0000;
            pipe2_x          <= 10'd0;
            pipe2_y          <= 9'd0;
            pipe2_w_q        <= 32'd0;
            pipe2_tex_addr   <= 14'd0;
            draw_pipe_valid  <= 1'b0;
            draw_pipe_inside <= 1'b0;
            draw_pipe_ztest  <= 1'b0;
            draw_pipe_textured <= 1'b0;
            draw_pipe_alpha_key <= 1'b0;
            draw_pipe_alpha <= 2'd0;
            draw_pipe_fog    <= 1'b0;
            draw_pipe_light_bank <= 2'd0;
            draw_pipe_tex_or_color <= 8'd0;
            draw_pipe_addr   <= 16'd0;
            draw_pipe_z      <= 16'd0;
            draw_pipe_z_ref  <= 16'd0;
            draw_pipe_dst_rgb565 <= 16'h0000;
            draw_pipe_x      <= 10'd0;
            draw_pipe_y      <= 9'd0;
            draw_pipe_w_q    <= 32'd0;
            draw_is_band_primer <= 1'b0;
            pal_rd_valid     <= 1'b0;
            pal_rd_pass      <= 1'b0;
            pal_rd_ztest     <= 1'b0;
            pal_rd_alpha     <= 2'd0;
            pal_rd_fog       <= 1'b0;
            pal_rd_addr      <= 16'd0;
            pal_rd_z         <= 16'd0;
            pal_rd_src_addr  <= 8'd0;
            pal_rd_fog_addr  <= 8'd0;
            pal_rd_dst_rgb565 <= 16'h0000;
            pal_rd_w_q       <= 32'd0;
            pal_rd_ray_scale_q16 <= 34'd0;
            plr_valid        <= 1'b0;
            plr_pass         <= 1'b0;
            plr_ztest        <= 1'b0;
            plr_alpha        <= 2'd0;
            plr_fog          <= 1'b0;
            plr_addr         <= 16'd0;
            plr_z            <= 16'd0;
            plr_src_rgb      <= 24'h000000;
            plr_dst_rgb565   <= 16'h0000;
            plr_fog_rgb      <= 24'h000000;
            plr_w_q          <= 32'd0;
            plr_ray_scale_q16 <= 34'd0;
            fog0_valid       <= 1'b0;
            fog0_pass        <= 1'b0;
            fog0_ztest       <= 1'b0;
            fog0_alpha       <= 2'd0;
            fog0_fog         <= 1'b0;
            fog0_addr        <= 16'd0;
            fog0_z           <= 16'd0;
            fog0_src_rgb565  <= 16'h0000;
            fog0_dst_rgb565  <= 16'h0000;
            fog0_fog_rgb565  <= 16'h0000;
            fog0_w_q         <= 32'd0;
            fog0_ray_scale_q16 <= 34'd0;
            fog1_valid       <= 1'b0;
            fog1_pass        <= 1'b0;
            fog1_ztest       <= 1'b0;
            fog1_alpha       <= 2'd0;
            fog1_fog         <= 1'b0;
            fog1_addr        <= 16'd0;
            fog1_z           <= 16'd0;
            fog1_src_rgb565  <= 16'h0000;
            fog1_dst_rgb565  <= 16'h0000;
            fog1_fog_rgb565  <= 16'h0000;
            fog1_radial_q8_8 <= 16'd0;
            commit_valid     <= 1'b0;
            commit_pass      <= 1'b0;
            commit_ztest     <= 1'b0;
            commit_addr      <= 16'd0;
            commit_z         <= 16'd0;
            commit_color     <= 16'h0000;
            pipe0_valid_o    <= 1'b0;
            recip0_valid_o   <= 1'b0;
            recip1_valid_o   <= 1'b0;
            recip2_valid_o   <= 1'b0;
            pipe1_valid_o    <= 1'b0;
            tex0_valid_o     <= 1'b0;
            pipe2_valid_o    <= 1'b0;
            draw_pipe_valid_o <= 1'b0;
            pal_rd_valid_o   <= 1'b0;
            plr_valid_o      <= 1'b0;
            fog0_valid_o     <= 1'b0;
            fog1_valid_o     <= 1'b0;
            commit_valid_o   <= 1'b0;
            scan_rgb565_r    <= 16'h0000;
            scan_line0_ready <= 1'b0;
            scan_line1_ready <= 1'b0;
            scan_line2_ready <= 1'b0;
            scan_line0_row   <= 9'd0;
            scan_line1_row   <= 9'd0;
            scan_line2_row   <= 9'd0;
            scan_active_bank <= 2'd0;
            scan_active_valid <= 1'b0;
            scan_active_row  <= 9'd0;
            scan_fill_active <= 1'b0;
            scan_fill_armed  <= 1'b0;
            scan_fill_load_pending <= 1'b0;
            scan_fill_bank   <= 2'd0;
            scan_fill_row    <= 9'd0;
            scan_fill_base_words <= 25'd0;
            scan_fill_store_idx <= 10'd0;
            scan_rd_capture <= 1'b0;
            scan_late_count <= 16'd0;
            sdram_rd_addr_cfg <= 25'd0;
            sdram_rd_max_addr_cfg <= 25'd0;
            sdram_rd_load_pulse <= 1'b0;
            sdram_rd_load_stretch_req <= 1'b0;
            sdram_rd_load_hold  <= 4'd0;
            cache_band_index <= 3'd0;
            cache_target_band <= 3'd0;
            band_index_cfg <= 3'd0;
            band_begin_pending <= 1'b0;
            band_flush_pending <= 1'b0;
            cache_valid <= 1'b0;
            cache_dirty <= 1'b0;
            cache_draw_dirty <= 1'b0;
            cache_band_valid <= 8'h00;
            cache_resume_draw <= 1'b0;
            cache_final_flush <= 1'b0;
            cache_maint_addr <= 16'd0;
            cache_pixels_total <= 16'd0;
            cache_init_x <= 10'd0;
            cache_init_sky_row_count <= 5'd0;
            cache_init_sky_palette <= PAL_SKY_GRADIENT_BASE;
            cache_words_issued <= 16'd0;
            cache_words_done <= 16'd0;
            cache_fetch_inflight <= 1'b0;
            cache_flush_load_pending <= 1'b0;
            cache_drain_count <= 8'd0;
            cache_word_pending_valid <= 1'b0;
            cache_word_pending <= 16'd0;
            cache_load_is_z <= 1'b0;
            cache_rd_capture <= 1'b0;
            sdram_wr_addr_cfg <= 25'd0;
            sdram_wr_max_addr_cfg <= 25'd0;
            sdram_wr_load_pulse <= 1'b0;
            /* Ping-pong cache reset */
            draw_cache_sel <= 1'b0;
            fb_A_rd_sel_q <= 1'b0;
            fb_B_rd_sel_q <= 1'b0;
            z_A_rd_sel_q <= 1'b0;
            z_B_rd_sel_q <= 1'b0;
            flush_active <= 1'b0;
            flush_maint_addr <= 16'd0;
            flush_pixels_total <= 16'd0;
            flush_words_issued <= 16'd0;
            flush_words_done <= 16'd0;
            flush_fetch_inflight <= 1'b0;
            flush_word_pending_valid <= 1'b0;
            flush_word_pending <= 16'd0;
            flush_load_pending <= 1'b0;
            flush_drain_count <= 8'd0;
            flush_band_index <= 3'd0;
            flush_sdram_wr_addr <= 25'd0;
            flush_sdram_wr_max_addr <= 25'd0;
            flush_cache_sel <= 1'b0;
            flush_generated_sky <= 1'b0;
            flush_sky_x <= 10'd0;
            flush_sky_row_count <= 5'd0;
            flush_sky_palette <= PAL_SKY_GRADIENT_BASE;
            draw_row_inside <= 1'b0;
            sdram_powerup_counter <= 18'd0;
            sdram_init_wait_counter <= 16'd0;
            sdram_ctrl_reset_n <= 1'b0;
            sdram_ready <= 1'b0;
            vga_vs_d <= 1'b1;
            scan_visible_r <= 1'b0;
            for (ei = 0; ei < 4; ei = ei + 1) begin
                edge_A[ei] <= 32'sd0;
                edge_B[ei] <= 32'sd0;
                edge_C[ei] <= 32'sd0;
                edge_row_val[ei] <= 64'sd0;
                edge_cur_val[ei] <= 64'sd0;
            end
        end else begin
            /*
             * Texture read no longer happens here -- voxel_texture_rom
             * drives tex_rd_data directly from pipe2_tex_addr with a
             * fixed 1-cycle latency. Leaving it as an always_ff
             * assignment let Quartus silently stack an extra flop on
             * top of the M10K's internal output register, which pushed
             * tex_rd_data one cycle late on real hardware.
             */
            vga_vs_d <= VGA_VS;
            scan_rgb565_r <= scan_rgb565_now;
            scan_visible_r <= scan_visible_now;
            sdram_wr_load_pulse <= 1'b0;
            sdram_rd_load_pulse <= 1'b0;
            sdram_rd_load_stretch_req <= 1'b0;
            scan_rd_capture <= scan_rd_pop;
            cache_rd_capture <= cache_rd_pop;
            fb_A_rd_sel_q <= fb_A_e_rd_addr[0];
            fb_B_rd_sel_q <= fb_B_e_rd_addr[0];
            z_A_rd_sel_q <= z_A_e_rd_addr[0];
            z_B_rd_sel_q <= z_B_e_rd_addr[0];

            /*
             * Whenever a stretched pulse is requested (sampled here as the
             * previous-cycle scheduler result), prime the hold counter so
             * RD_LOAD stays high for 3 additional REF_CLK cycles (4 total).
             */
            if (sdram_rd_load_pulse && sdram_rd_load_stretch_req) begin
                sdram_rd_load_hold <= 4'd3;
            end else if (sdram_rd_load_hold != 4'd0) begin
                sdram_rd_load_hold <= sdram_rd_load_hold - 4'd1;
            end

            if (!sdram_ctrl_reset_n) begin
                if (sdram_powerup_counter == SDRAM_POWERUP_HOLD_LAST) begin
                    sdram_ctrl_reset_n <= 1'b1;
                end else begin
                    sdram_powerup_counter <= sdram_powerup_counter + 18'd1;
                end
            end else if (!sdram_ready) begin
                if (sdram_init_wait_counter == SDRAM_INIT_WAIT_LAST) begin
                    sdram_ready <= 1'b1;
                end else begin
                    sdram_init_wait_counter <= sdram_init_wait_counter + 16'd1;
                end
            end

            if (vsync_pulse) begin
                frame_count <= frame_count + 32'd1;
                scan_line0_ready <= 1'b0;
                scan_line1_ready <= 1'b0;
                scan_line2_ready <= 1'b0;
                scan_line0_row <= 9'd0;
                scan_line1_row <= 9'd0;
                scan_line2_row <= 9'd0;
                scan_active_bank <= 2'd0;
                scan_active_valid <= 1'b0;
                scan_active_row <= 9'd0;
                scan_fill_active <= 1'b0;
                scan_fill_armed <= 1'b0;
                scan_fill_load_pending <= 1'b0;
                scan_fill_bank <= 2'd0;
                scan_fill_row <= 9'd0;
                scan_fill_base_words <= 25'd0;
                scan_fill_store_idx <= 10'd0;
                scan_rd_capture <= 1'b0;

                if (copy_complete_pending) begin
                    display_sel <= copy_target_sel;
                    display_valid <= 1'b1;
                    ctrl_flp_pending <= 1'b0;
                    copy_complete_pending <= 1'b0;
                    vsy_latch <= 1'b1;
                    if (sdram_ready) begin
                        scan_fill_active <= 1'b1;
                        scan_fill_armed <= 1'b1;
                        scan_fill_load_pending <= 1'b0;
                        scan_fill_bank <= 2'd0;
                        scan_fill_row <= 9'd0;
                        scan_fill_base_words <= copy_target_base_words;
                        scan_fill_store_idx <= 10'd0;
                        sdram_rd_addr_cfg <= copy_target_base_words;
                        sdram_rd_max_addr_cfg <= copy_target_base_words + READ_BURST_WORDS_25;
                        sdram_rd_load_pulse <= 1'b1;
                        sdram_rd_load_stretch_req <= 1'b1;
                    end
                end else begin
                    vsy_latch <= !ctrl_flp_pending;
                    if (display_valid && sdram_ready) begin
                        scan_fill_active <= 1'b1;
                        scan_fill_armed <= 1'b1;
                        scan_fill_load_pending <= 1'b0;
                        scan_fill_bank <= 2'd0;
                        scan_fill_row <= 9'd0;
                        scan_fill_base_words <= display_base_words;
                        scan_fill_store_idx <= 10'd0;
                        sdram_rd_addr_cfg <= display_base_words;
                        sdram_rd_max_addr_cfg <= display_base_words + READ_BURST_WORDS_25;
                        sdram_rd_load_pulse <= 1'b1;
                        sdram_rd_load_stretch_req <= 1'b1;
                    end
                end
            end

            if (wr) begin
                case (address)
                    ADDR_CONTROL: begin
                        ctrl_en  <= writedata[0];
                        ctrl_ien <= writedata[2];
                        if (writedata[1]) begin
                            ctrl_flp_pending <= 1'b1;
                            vsy_latch        <= 1'b0;
                        end
                        if (writedata[3]) begin
                            clear_pending <= 1'b1;
                        end
                    end
                    ADDR_PAL_ADDR: pal_addr <= writedata[7:0];
                    ADDR_PAL_DATA: begin
                        palette[pal_addr] <= writedata[23:0];
                        pal_addr <= pal_addr + 8'd1;
                    end
                    ADDR_FOG_RANGE: begin
                        fog_start_dist <= writedata[15:0];
                        fog_end_dist   <= writedata[31:16];
                    end
                    ADDR_FOG_CTRL: begin
                        fog_color       <= writedata[7:0];
                        fog_enable      <= writedata[8];
                        fog_inv_proj_sq <= writedata[31:16];
                    end
                    ADDR_EXTMEM_CTRL: extmem_ctrl <= writedata;
                    ADDR_EXTMEM_FRONT: extmem_front_base <= writedata;
                    ADDR_EXTMEM_BACK: extmem_back_base <= writedata;
                    ADDR_EXTMEM_STRIDE: extmem_stride_bytes <= writedata;
                    ADDR_EXTMEM_TILE: extmem_tile_cfg <= writedata;
                    ADDR_BAND_INDEX: begin
                        band_index_cfg <= writedata[2:0];
                    end
                    ADDR_BAND_CTRL: begin
                        if (writedata[0])
                            band_begin_pending <= 1'b1;
                        if (writedata[1])
                            band_flush_pending <= 1'b1;
                    end
                    default: ;
                endcase
            end

            if (fifo_push_req)
                fifo_mem[fifo_wr_ptr] <= writedata;
            if (fifo_pop_req)
                desc_words[fetch_count[4:0]] <= fifo_head;

            case ({fifo_push_req, fifo_pop_req})
                2'b10: begin
                    fifo_wr_ptr <= fifo_wr_ptr + 11'd1;
                    fifo_count  <= fifo_count + 12'd1;
                end
                2'b01: begin
                    fifo_rd_ptr <= fifo_rd_ptr + 11'd1;
                    fifo_count  <= fifo_count - 12'd1;
                end
                2'b11: begin
                    fifo_wr_ptr <= fifo_wr_ptr + 11'd1;
                    fifo_rd_ptr <= fifo_rd_ptr + 11'd1;
                end
                default: ;
            endcase

            if (!vsync_pulse && !cache_load_state && !cache_drain_state) begin
                if (scan_hblank_start && display_valid && scan_active_valid &&
                    scan_target_valid && !scan_target_line_ready &&
                    (scan_late_count != 16'hffff))
                    scan_late_count <= scan_late_count + 16'd1;

                if (scan_fill_load_pending) begin
                    scan_fill_load_pending <= 1'b0;
                    sdram_rd_addr_cfg <= scan_fill_base_words +
                                         {15'd0, scan_fill_store_idx};
                    sdram_rd_max_addr_cfg <= scan_fill_base_words +
                                             {15'd0, scan_fill_store_idx} +
                                             READ_BURST_WORDS_25;
                    sdram_rd_load_pulse <= 1'b1;
                end else if (scan_fill_armed && !sdram_rd_load_out && !sdram_rd_empty) begin
                    scan_fill_armed <= 1'b0;
                end

                if (scan_rd_capture) begin
                    case (scan_fill_bank)
                        2'd0: scan_linebuf0[scan_fill_store_idx] <= sdram_rd_data;
                        2'd1: scan_linebuf1[scan_fill_store_idx] <= sdram_rd_data;
                        default: scan_linebuf2[scan_fill_store_idx] <= sdram_rd_data;
                    endcase

                    if (scan_fill_line_done) begin
                        /*
                         * If the SDRAM RD FIFO output register coughs up a
                         * stale word on the first pop after RD_LOAD, it
                         * lands in linebuf[0] and looks like the rightmost
                         * column wrapping to the left edge. Once the full
                         * line is resident, copy column 1 over column 0 as
                         * a one-pixel edge guard. This does not shift the
                         * scanout timing or touch the actual framebuffer.
                         */
                        case (scan_fill_bank)
                            2'd0: scan_linebuf0[10'd0] <= scan_linebuf0[10'd1];
                            2'd1: scan_linebuf1[10'd0] <= scan_linebuf1[10'd1];
                            default: scan_linebuf2[10'd0] <= scan_linebuf2[10'd1];
                        endcase
                        scan_fill_active <= 1'b0;
                        scan_fill_armed <= 1'b0;
                        case (scan_fill_bank)
                            2'd0: begin
                                scan_line0_ready <= 1'b1;
                                scan_line0_row <= scan_fill_row;
                            end
                            2'd1: begin
                                scan_line1_ready <= 1'b1;
                                scan_line1_row <= scan_fill_row;
                            end
                            default: begin
                                scan_line2_ready <= 1'b1;
                                scan_line2_row <= scan_fill_row;
                            end
                        endcase

                        if (!scan_active_valid) begin
                            scan_active_valid <= 1'b1;
                            scan_active_bank <= scan_fill_bank;
                            scan_active_row <= scan_fill_row;
                        end
                    end else begin
                        scan_fill_store_idx <= scan_fill_store_idx + 10'd1;
                        if (scan_fill_chunk_done) begin
                            scan_fill_armed <= 1'b1;
                            scan_fill_load_pending <= 1'b1;
                        end
                    end
                end

                /* Only change the displayed linebuffer in horizontal blank.
                 * If a line is late, keep showing the last complete line for
                 * this scanline instead of switching partway across the row. */
                if (scan_hblank_window && display_valid && scan_active_valid &&
                    (scan_active_row != scan_target_row)) begin
                    if (scan_line0_ready && (scan_line0_row == scan_target_row)) begin
                        scan_active_bank <= 2'd0;
                        scan_active_row <= scan_target_row;
                    end else if (scan_line1_ready && (scan_line1_row == scan_target_row)) begin
                        scan_active_bank <= 2'd1;
                        scan_active_row <= scan_target_row;
                    end else if (scan_line2_ready && (scan_line2_row == scan_target_row)) begin
                        scan_active_bank <= 2'd2;
                        scan_active_row <= scan_target_row;
                    end else if ((scan_active_row != scan_current_row) &&
                                 scan_line0_ready && (scan_line0_row == scan_current_row)) begin
                        scan_active_bank <= 2'd0;
                        scan_active_row <= scan_current_row;
                    end else if ((scan_active_row != scan_current_row) &&
                                 scan_line1_ready && (scan_line1_row == scan_current_row)) begin
                        scan_active_bank <= 2'd1;
                        scan_active_row <= scan_current_row;
                    end else if ((scan_active_row != scan_current_row) &&
                                 scan_line2_ready && (scan_line2_row == scan_current_row)) begin
                        scan_active_bank <= 2'd2;
                        scan_active_row <= scan_current_row;
                    end else if (scan_line0_ready && (scan_line0_row == scan_active_next_row)) begin
                        scan_active_bank <= 2'd0;
                        scan_active_row <= scan_active_next_row;
                    end else if (scan_line1_ready && (scan_line1_row == scan_active_next_row)) begin
                        scan_active_bank <= 2'd1;
                        scan_active_row <= scan_active_next_row;
                    end else if (scan_line2_ready && (scan_line2_row == scan_active_next_row)) begin
                        scan_active_bank <= 2'd2;
                        scan_active_row <= scan_active_next_row;
                    end
                end

                if (scan_prefetch_req) begin
                    case (scan_prefetch_bank)
                    2'd0: begin
                        scan_fill_active <= 1'b1;
                        scan_fill_armed <= 1'b1;
                        scan_fill_load_pending <= 1'b0;
                        scan_fill_bank <= 2'd0;
                        scan_fill_row <= scan_prefetch_row;
                        scan_fill_base_words <= scan_prefetch_base_words;
                        scan_fill_store_idx <= 10'd0;
                        scan_line0_ready <= 1'b0;
                        sdram_rd_addr_cfg <= scan_prefetch_base_words;
                        sdram_rd_max_addr_cfg <= scan_prefetch_base_words + READ_BURST_WORDS_25;
                        sdram_rd_load_pulse <= 1'b1;
                        sdram_rd_load_stretch_req <= 1'b1;
                    end
                    2'd1: begin
                        scan_fill_active <= 1'b1;
                        scan_fill_armed <= 1'b1;
                        scan_fill_load_pending <= 1'b0;
                        scan_fill_bank <= 2'd1;
                        scan_fill_row <= scan_prefetch_row;
                        scan_fill_base_words <= scan_prefetch_base_words;
                        scan_fill_store_idx <= 10'd0;
                        scan_line1_ready <= 1'b0;
                        sdram_rd_addr_cfg <= scan_prefetch_base_words;
                        sdram_rd_max_addr_cfg <= scan_prefetch_base_words + READ_BURST_WORDS_25;
                        sdram_rd_load_pulse <= 1'b1;
                        sdram_rd_load_stretch_req <= 1'b1;
                    end
                    default: begin
                        scan_fill_active <= 1'b1;
                        scan_fill_armed <= 1'b1;
                        scan_fill_load_pending <= 1'b0;
                        scan_fill_bank <= 2'd2;
                        scan_fill_row <= scan_prefetch_row;
                        scan_fill_base_words <= scan_prefetch_base_words;
                        scan_fill_store_idx <= 10'd0;
                        scan_line2_ready <= 1'b0;
                        sdram_rd_addr_cfg <= scan_prefetch_base_words;
                        sdram_rd_max_addr_cfg <= scan_prefetch_base_words + READ_BURST_WORDS_25;
                        sdram_rd_load_pulse <= 1'b1;
                        sdram_rd_load_stretch_req <= 1'b1;
                    end
                    endcase
                end
            end

            /* Main-FSM flush: used for ST_CACHE_FLUSH_COLOR/Z only */
            if (main_flush_wr_push) begin
                cache_word_pending_valid <= 1'b0;
                cache_words_done <= cache_words_done + 16'd1;
            end

            if (cache_flush_state) begin
                if (cache_fetch_inflight &&
                    (!cache_word_pending_valid || main_flush_wr_push)) begin
                    cache_word_pending <= (state == ST_CACHE_FLUSH_Z) ?
                                          z_rd_data : fb_back_rd_data;
                    cache_word_pending_valid <= 1'b1;
                    cache_fetch_inflight <= 1'b0;
                end

                if (cache_issue_read) begin
                    cache_maint_addr <= cache_words_issued;
                    cache_words_issued <= cache_words_issued + 16'd1;
                    cache_fetch_inflight <= 1'b1;
                end
            end

            /* ── Background flush controller ─────────────────────────── */
            /* Runs independently of the main FSM. Reads from the inactive
             * cache and streams pixels to the SDRAM write FIFO. */
            if (bg_flush_wr_push) begin
                flush_word_pending_valid <= 1'b0;
                flush_words_done <= flush_words_done + 16'd1;
            end

            if (flush_active && !cache_flush_state) begin
                /* Clear load pending once WR_LOAD has safely reset the FIFO. */
                if (bg_flush_wr_load_req)
                    flush_load_pending <= 1'b0;

                /* Capture read data after one-cycle latency */
                if (!flush_generated_sky && flush_fetch_inflight &&
                    (!flush_word_pending_valid || bg_flush_wr_push)) begin
                    flush_word_pending <= flush_fb_rd_data;
                    flush_word_pending_valid <= 1'b1;
                    flush_fetch_inflight <= 1'b0;
                end

                /* Issue next read from inactive cache */
                if (!flush_generated_sky && flush_can_issue_read) begin
                    flush_maint_addr <= flush_words_issued;
                    flush_words_issued <= flush_words_issued + 16'd1;
                    flush_fetch_inflight <= 1'b1;
                end

                /*
                 * Sky-only bands do not need the local cache as a flush source:
                 * ST_CACHE_INIT already wrote the same gradient into the cache,
                 * but no real draw committed over it. Generate the SDRAM stream
                 * directly so the next fast band can reuse either local cache
                 * without waiting for this flush to finish.
                 */
                if (flush_can_issue_sky) begin
                    flush_word_pending <= rgb888_to_rgb565(palette[flush_sky_palette]);
                    flush_word_pending_valid <= 1'b1;
                    flush_words_issued <= flush_words_issued + 16'd1;
                    if (flush_sky_x == 10'd639) begin
                        flush_sky_x <= 10'd0;
                        if (flush_sky_row_count == 5'd19) begin
                            flush_sky_row_count <= 5'd0;
                            if (flush_sky_palette != PAL_SKY_GRADIENT_LAST)
                                flush_sky_palette <= flush_sky_palette + 8'd1;
                        end else begin
                            flush_sky_row_count <= flush_sky_row_count + 5'd1;
                        end
                    end else begin
                        flush_sky_x <= flush_sky_x + 10'd1;
                    end
                end

                /* Flush complete: all words pushed to SDRAM, FIFO drained,
                 * and the SDRAM controller has had time to finish the final
                 * burst it already pulled out of the FIFO. */
                if ((flush_words_done == flush_pixels_total) &&
                    !flush_word_pending_valid &&
                    !flush_fetch_inflight &&
                    (sdram_wr_use[8:0] == 9'd0)) begin
                    if (flush_drain_count == COPY_DRAIN_CYCLES) begin
                        flush_active <= 1'b0;
                        flush_generated_sky <= 1'b0;
                        cache_band_valid[flush_band_index] <= 1'b1;
                        flush_drain_count <= 8'd0;
                    end else begin
                        flush_drain_count <= flush_drain_count + 8'd1;
                    end
                end else begin
                    flush_drain_count <= 8'd0;
                end
            end

            if (cache_load_state) begin
                if (cache_rd_capture) begin
                    cache_maint_addr <= cache_maint_addr + 16'd1;
                    cache_words_done <= cache_words_done + 16'd1;
                end
            end

            /* pal_rd stage: register the palette source/fog addresses
             * (both derived combinationally from draw_pipe_*) along
             * with every other field fog0 needs. Having a dedicated
             * address flop before the palette read lets Quartus pick
             * either MLAB or M10K for the palette array without
             * introducing a hidden 1-cycle skew: with MLAB the read
             * is async out of pal_rd_*_addr and the plr stage flops
             * it; with M10K the address flop is what the primitive
             * expects internally and plr simply reads the next-cycle
             * data. Either way, plr_src_rgb/plr_fog_rgb stay
             * aligned with plr_pass/plr_alpha/... */
            pal_rd_valid       <= draw_pipe_valid;
            pal_rd_pass        <= draw_commit_pass;
            pal_rd_ztest       <= draw_pipe_ztest;
            pal_rd_alpha       <= draw_pipe_alpha;
            pal_rd_fog         <= draw_pipe_fog;
            pal_rd_addr        <= draw_pipe_addr;
            pal_rd_z           <= draw_pipe_z;
            pal_rd_src_addr    <= palette_src_addr;
            pal_rd_fog_addr    <= fog_color;
            pal_rd_dst_rgb565  <= draw_pipe_dst_rgb565;
            pal_rd_w_q         <= draw_pipe_w_q;
            pal_rd_ray_scale_q16 <= draw_pipe_ray_scale_q16;
            pal_rd_valid_o       <= draw_pipe_valid_o;
            pal_rd_pass_o        <= draw_commit_pass_o;
            pal_rd_ztest_o       <= draw_pipe_ztest_o;
            pal_rd_alpha_o       <= draw_pipe_alpha_o;
            pal_rd_fog_o         <= draw_pipe_fog_o;
            pal_rd_addr_o        <= draw_pipe_addr_o;
            pal_rd_z_o           <= draw_pipe_z_o;
            pal_rd_src_addr_o    <= palette_src_addr_o;
            pal_rd_fog_addr_o    <= fog_color;
            pal_rd_dst_rgb565_o  <= draw_pipe_dst_rgb565_o;
            pal_rd_w_q_o         <= draw_pipe_w_q_o;
            pal_rd_ray_scale_q16_o <= draw_pipe_ray_scale_q16_o;

            /* plr stage: register the palette lookup output so every
             * fog0 input arrives on the same cycle. The combinational
             * chain here is now just palette[pal_rd_*_addr] (an array
             * index with a registered address), which is MUCH shorter
             * than the old tex_rd_data -> apply_light_bank -> palette
             * -> rgb888_to_rgb565 path we used to have feeding fog0
             * directly. */
            plr_valid          <= pal_rd_valid;
            plr_pass           <= pal_rd_pass;
            plr_ztest          <= pal_rd_ztest;
            plr_alpha          <= pal_rd_alpha;
            plr_fog            <= pal_rd_fog;
            plr_addr           <= pal_rd_addr;
            plr_z              <= pal_rd_z;
            plr_src_rgb        <= palette[pal_rd_src_addr];
            plr_dst_rgb565     <= pal_rd_dst_rgb565;
            plr_fog_rgb        <= palette[pal_rd_fog_addr];
            plr_w_q            <= pal_rd_w_q;
            plr_ray_scale_q16  <= pal_rd_ray_scale_q16;
            plr_valid_o          <= pal_rd_valid_o;
            plr_pass_o           <= pal_rd_pass_o;
            plr_ztest_o          <= pal_rd_ztest_o;
            plr_alpha_o          <= pal_rd_alpha_o;
            plr_fog_o            <= pal_rd_fog_o;
            plr_addr_o           <= pal_rd_addr_o;
            plr_z_o              <= pal_rd_z_o;
            plr_src_rgb_o        <= palette[pal_rd_src_addr_o];
            plr_dst_rgb565_o     <= pal_rd_dst_rgb565_o;
            plr_fog_rgb_o        <= palette[pal_rd_fog_addr_o];
            plr_w_q_o            <= pal_rd_w_q_o;
            plr_ray_scale_q16_o  <= pal_rd_ray_scale_q16_o;

            fog0_valid <= plr_valid;
            fog0_pass <= plr_pass;
            fog0_ztest <= plr_ztest;
            fog0_alpha <= plr_alpha;
            fog0_fog <= plr_fog;
            fog0_addr <= plr_addr;
            fog0_z <= plr_z;
            fog0_src_rgb565 <= plr_src_rgb565;
            fog0_dst_rgb565 <= plr_dst_rgb565;
            fog0_fog_rgb565 <= plr_fog_rgb565;
            fog0_w_q <= plr_w_q;
            fog0_ray_scale_q16 <= plr_ray_scale_q16;
            fog0_valid_o <= plr_valid_o;
            fog0_pass_o <= plr_pass_o;
            fog0_ztest_o <= plr_ztest_o;
            fog0_alpha_o <= plr_alpha_o;
            fog0_fog_o <= plr_fog_o;
            fog0_addr_o <= plr_addr_o;
            fog0_z_o <= plr_z_o;
            fog0_src_rgb565_o <= plr_src_rgb565_o;
            fog0_dst_rgb565_o <= plr_dst_rgb565_o;
            fog0_fog_rgb565_o <= plr_fog_rgb565_o;
            fog0_w_q_o <= plr_w_q_o;
            fog0_ray_scale_q16_o <= plr_ray_scale_q16_o;

            fog1_valid <= fog0_valid;
            fog1_pass <= fog0_pass;
            fog1_ztest <= fog0_ztest;
            fog1_alpha <= fog0_alpha;
            fog1_fog <= fog0_fog;
            fog1_addr <= fog0_addr;
            fog1_z <= fog0_z;
            fog1_src_rgb565 <= fog0_src_rgb565;
            fog1_dst_rgb565 <= fog0_dst_rgb565;
            fog1_fog_rgb565 <= fog0_fog_rgb565;
            fog1_radial_q8_8 <= fog0_radial_q8_8;
            fog1_valid_o <= fog0_valid_o;
            fog1_pass_o <= fog0_pass_o;
            fog1_ztest_o <= fog0_ztest_o;
            fog1_alpha_o <= fog0_alpha_o;
            fog1_fog_o <= fog0_fog_o;
            fog1_addr_o <= fog0_addr_o;
            fog1_z_o <= fog0_z_o;
            fog1_src_rgb565_o <= fog0_src_rgb565_o;
            fog1_dst_rgb565_o <= fog0_dst_rgb565_o;
            fog1_fog_rgb565_o <= fog0_fog_rgb565_o;
            fog1_radial_q8_8_o <= fog0_radial_q8_8_o;

            commit_valid <= fog1_valid;
            commit_pass <= fog1_pass;
            commit_ztest <= fog1_ztest;
            commit_addr <= fog1_addr;
            commit_z <= fog1_z;
            commit_color <= fog1_out_rgb565;
            commit_valid_o <= fog1_valid_o;
            commit_pass_o <= fog1_pass_o;
            commit_ztest_o <= fog1_ztest_o;
            commit_addr_o <= fog1_addr_o;
            commit_z_o <= fog1_z_o;
            commit_color_o <= fog1_out_rgb565_o;
            if ((state == ST_DRAW || state == ST_DRAW_FLUSH) &&
                ((commit_valid && commit_pass) ||
                 (commit_valid_o && commit_pass_o))) begin
                cache_dirty <= 1'b1;
                if (!draw_is_band_primer)
                    cache_draw_dirty <= 1'b1;
            end

            case (state)
                ST_IDLE: begin
                    fetch_count <= 6'd0;
                    draw_flush_count <= 4'd0;
                    pipe0_valid <= 1'b0;
                    recip0_valid <= 1'b0;
                    recip1_valid <= 1'b0;
                    recip2_valid <= 1'b0;
                    pipe1_valid <= 1'b0;
                    tex0_valid <= 1'b0;
                    pipe2_valid <= 1'b0;
                    draw_pipe_valid <= 1'b0;
                    pal_rd_valid <= 1'b0;
                    plr_valid <= 1'b0;
                    fog0_valid <= 1'b0;
                    fog1_valid <= 1'b0;
                    commit_valid <= 1'b0;
                    pipe0_valid_o <= 1'b0;
                    recip0_valid_o <= 1'b0;
                    recip1_valid_o <= 1'b0;
                    recip2_valid_o <= 1'b0;
                    pipe1_valid_o <= 1'b0;
                    tex0_valid_o <= 1'b0;
                    pipe2_valid_o <= 1'b0;
                    draw_pipe_valid_o <= 1'b0;
                    pal_rd_valid_o <= 1'b0;
                    plr_valid_o <= 1'b0;
                    fog0_valid_o <= 1'b0;
                    fog1_valid_o <= 1'b0;
                    commit_valid_o <= 1'b0;
                    if (clear_pending) begin
                        state         <= ST_CLEAR;
                        clear_pending <= 1'b0;
                        clear_addr    <= 16'd0;
                    end else if (band_begin_pending && band_begin_cache_available) begin
                        /*
                         * There are only two local caches. We can overlap one
                         * band's background flush with the next band's draw,
                         * but a second fast BEGIN_BAND would toggle back into
                         * the cache still being flushed. Sky/ground views hit
                         * this hard because bands complete almost immediately.
                         *
                         * The gate is in the outer else-if so that when it
                         * fails, the chain falls through to the band_flush
                         * branch below — otherwise a queued flush could be
                         * stranded behind a begin we cannot service yet.
                         */
                        band_begin_pending <= 1'b0;
                        cache_target_band <= band_index_cfg;
                        cache_band_index <= band_index_cfg;
                        cache_pixels_total <= band_pixel_count(band_index_cfg);
                        cache_maint_addr <= 16'd0;
                        cache_init_x <= 10'd0;
                        cache_init_sky_row_count <= sky_clear_start_row_count(band_index_cfg);
                        cache_init_sky_palette <= sky_clear_start_palette(band_index_cfg);
                        cache_valid <= 1'b0;
                        cache_dirty <= 1'b0;
                        cache_draw_dirty <= 1'b0;
                        /* Toggle cache selector: draw into the other cache */
                        draw_cache_sel <= ~draw_cache_sel;
                        state <= ST_CACHE_INIT;
                    end else if (band_flush_pending && !flush_active) begin
                        /*
                         * end_band() polls FEM=1 before setting band_flush_pending,
                         * so band-N's descriptors are guaranteed already drained
                         * from the FIFO. Anything in the FIFO at this point
                         * belongs to band N+1 (SW pipelined begin_band(N+1) and
                         * started pushing descriptors while we were waiting on
                         * the prior flush). Don't gate this branch on
                         * fifo_count==0 — those next-band descriptors are
                         * blocked from being fetched by the !band_flush_pending
                         * gate on the ST_FETCH branch below, and they will
                         * drain after this branch clears band_flush_pending and
                         * branch 2 consumes BEGIN_(N+1) to advance
                         * cache_band_index. Adding fifo_count==0 here would
                         * deadlock: branch 5 can't drain (waiting on this branch
                         * to clear band_flush_pending), and this branch can't
                         * fire (waiting for the FIFO to drain).
                         */
                        band_flush_pending <= 1'b0;
                        if (cache_valid && cache_dirty) begin
                            /* Kick background flush on the current active cache.
                             * The next BEGIN_BAND will toggle draw_cache_sel,
                             * so the rasterizer will switch to the other cache
                             * while this one flushes in the background. */
                            flush_active <= 1'b1;
                            flush_band_index <= cache_band_index;
                            flush_pixels_total <= band_pixel_count(cache_band_index);
                            flush_maint_addr <= 16'd0;
                            flush_words_issued <= 16'd0;
                            flush_words_done <= 16'd0;
                            flush_fetch_inflight <= 1'b0;
                            flush_word_pending_valid <= 1'b0;
                            flush_load_pending <= 1'b1;
                            flush_drain_count <= 8'd0;
                            flush_cache_sel <= draw_cache_sel;
                            flush_generated_sky <= sky_gradient_clear_enabled &&
                                                   !cache_draw_dirty;
                            flush_sky_x <= 10'd0;
                            flush_sky_row_count <= sky_clear_start_row_count(cache_band_index);
                            flush_sky_palette <= sky_clear_start_palette(cache_band_index);
                            flush_sdram_wr_addr <= copy_target_base_words +
                                                   band_word_offset(cache_band_index);
                            flush_sdram_wr_max_addr <= copy_target_base_words +
                                                       band_word_offset(cache_band_index) +
                                                       band_word_count(cache_band_index);
                            cache_valid <= 1'b0;
                            cache_dirty <= 1'b0;
                            cache_draw_dirty <= 1'b0;
                        end
                        /* Don't enter ST_CACHE_FLUSH_COLOR — stay in ST_IDLE
                         * so the next BEGIN_BAND can proceed immediately */
                    end else if (ctrl_flp_pending && sdram_ready &&
                                !copy_complete_pending && !flush_active) begin
                        copy_complete_pending <= 1'b1;
                        cache_final_flush <= 1'b0;
                    end else if (ctrl_en && (fifo_count >= 12'd16) &&
                                 !band_flush_pending) begin
                        /*
                         * Don't pull descriptors while a band flush is queued.
                         * end_band() polls FEM before setting band_flush_pending,
                         * so the FIFO is supposed to be empty here — but
                         * begin_band(N+1) only polls BSY=0 (which excludes
                         * band_flush_pending after cff6eba). begin_band(N+1)
                         * can therefore return while band_flush_pending=1 from
                         * the prior end_band(N), and SW then pushes desc-(N+1)
                         * into the FIFO. cache_band_index is still N at that
                         * point (BEGIN_(N+1) is queued but blocked from
                         * consuming by the !band_flush_pending gate on
                         * branch 2), so without this gate ST_FETCH would draw
                         * desc-(N+1) into cache N. Wait until branch 3 kicks
                         * the band-N flush (clearing band_flush_pending) and
                         * branch 2 consumes BEGIN_(N+1) (updating
                         * cache_band_index to N+1) before fetching.
                         */
                        state       <= ST_FETCH;
                        fetch_count <= 6'd0;
                    end
                end

                ST_CLEAR: begin
                    copy_target_sel <= ~display_sel;
                    cache_valid <= 1'b0;
                    cache_dirty <= 1'b0;
                    cache_draw_dirty <= 1'b0;
                    cache_band_valid <= 8'h00;
                    cache_init_x <= 10'd0;
                    cache_init_sky_row_count <= 5'd0;
                    cache_init_sky_palette <= PAL_SKY_GRADIENT_BASE;
                    cache_resume_draw <= 1'b0;
                    cache_final_flush <= 1'b0;
                    band_begin_pending <= 1'b0;
                    band_flush_pending <= 1'b0;
                    copy_complete_pending <= 1'b0;
                    draw_cache_sel <= 1'b0;
                    /* Do not kill flush_active here — see the comment in
                     * ctrl_clear_write. Let the background flush drain
                     * naturally so the SDRAM framebuffer is fully written
                     * before scanout displays it. */
                    state <= ST_IDLE;
                end

                ST_FETCH: begin
                    if (fetch_count == fetch_target_words) begin
                        draw_x_min <= desc_x_min;
                        draw_x_max <= desc_x_max;
                        draw_y_min <= desc_y_min;
                        draw_y_max <= desc_y_max;
                        draw_row_base <= band_local_addr(desc_x_start_even, desc_y_min,
                                                         cache_band_index);
                        draw_x_cur <= desc_x_start_even;
                        draw_y_cur <= desc_y_min;
                        draw_row_inside <= 1'b0;
                        draw_tex_or_color <= desc_tex_or_color;
                        draw_flags <= desc_flags;
                        draw_is_band_primer <= desc_band_primer;
                        draw_z0    <= desc_z0;
                        draw_dz_dx <= desc_dz_dx;
                        draw_dz_dy <= desc_dz_dy;
                        if (desc_has_uv) begin
                            draw_uw_0  <= desc_uw_0;
                            draw_uw_dx <= desc_uw_dx;
                            draw_uw_dy <= desc_uw_dy;
                            draw_vw_0  <= desc_vw_0;
                            draw_vw_dx <= desc_vw_dx;
                            draw_vw_dy <= desc_vw_dy;
                            draw_iw_0  <= desc_iw_0;
                            draw_iw_dx <= desc_iw_dx;
                            draw_iw_dy <= desc_iw_dy;
                        end else begin
                            draw_uw_0  <= 32'sd0;
                            draw_uw_dx <= 32'sd0;
                            draw_uw_dy <= 32'sd0;
                            draw_vw_0  <= 32'sd0;
                            draw_vw_dx <= 32'sd0;
                            draw_vw_dy <= 32'sd0;
                            draw_iw_0  <= 32'sd0;
                            draw_iw_dx <= 32'sd0;
                            draw_iw_dy <= 32'sd0;
                        end
                        pipe0_valid <= 1'b0;
                        recip0_valid <= 1'b0;
                        recip1_valid <= 1'b0;
                        recip2_valid <= 1'b0;
                        pipe1_valid <= 1'b0;
                        tex0_valid <= 1'b0;
                        pipe2_valid <= 1'b0;
                        draw_pipe_valid <= 1'b0;
                        pal_rd_valid <= 1'b0;
                        plr_valid <= 1'b0;
                        fog0_valid <= 1'b0;
                        fog1_valid <= 1'b0;
                        commit_valid <= 1'b0;
                        pipe0_valid_o <= 1'b0;
                        recip0_valid_o <= 1'b0;
                        recip1_valid_o <= 1'b0;
                        recip2_valid_o <= 1'b0;
                        pipe1_valid_o <= 1'b0;
                        tex0_valid_o <= 1'b0;
                        pipe2_valid_o <= 1'b0;
                        draw_pipe_valid_o <= 1'b0;
                        pal_rd_valid_o <= 1'b0;
                        plr_valid_o <= 1'b0;
                        fog0_valid_o <= 1'b0;
                        fog1_valid_o <= 1'b0;
                        commit_valid_o <= 1'b0;
                        for (ei = 0; ei < 4; ei = ei + 1) begin
                            edge_A[ei] <= $signed(desc_words[2 + ei * 3]);
                            edge_B[ei] <= $signed(desc_words[3 + ei * 3]);
                            edge_C[ei] <= $signed(desc_words[4 + ei * 3]);
                        end

                        if ((desc_x_min > desc_x_max) || (desc_y_min > desc_y_max) ||
                            desc_redundant_sky_clear)
                            state <= ST_IDLE;
                        else
                            state <= ST_SETUP;
                    end else if (fifo_pop_req) begin
                        fetch_count <= fetch_count + 6'd1;
                    end
                end

                ST_SETUP: begin
                    edge_row_val[0] <= edge_eval0;
                    edge_cur_val[0] <= edge_eval0;
                    edge_row_val[1] <= edge_eval1;
                    edge_cur_val[1] <= edge_eval1;
                    edge_row_val[2] <= edge_eval2;
                    edge_cur_val[2] <= edge_eval2;
                    edge_row_val[3] <= edge_eval3;
                    edge_cur_val[3] <= edge_eval3;

                    z_row_val <= draw_z_start_val;
                    z_cur_val <= draw_z_start_val;
                    uw_row_val <= draw_uw_start_val;
                    uw_cur_val <= draw_uw_start_val;
                    vw_row_val <= draw_vw_start_val;
                    vw_cur_val <= draw_vw_start_val;
                    iw_row_val <= draw_iw_start_val;
                    iw_cur_val <= draw_iw_start_val;

                    state <= ST_DRAW;
                end

                ST_DRAW,
                ST_DRAW_FLUSH: begin
                    /*
                     * Stage 0: raster eval at the current pixel.
                     * Recip 0/1/2: normalize, lookup/interpolate, then rescale 1/w.
                     * Stage 1: perspective divide metadata aligned with final 1/w.
                     * Tex 0: register perspective texture-coordinate products.
                     * Stage 2: texel address formation.
                     * Stage 3/fog: commit metadata aligned with texture and fog latency.
                     */
                    recip0_valid <= pipe0_valid;
                    recip0_inside <= pipe0_inside;
                    recip0_ztest <= pipe0_ztest;
                    recip0_textured <= pipe0_textured;
                    recip0_alpha_key <= pipe0_alpha_key;
                    recip0_alpha <= pipe0_alpha;
                    recip0_fog <= pipe0_fog;
                    recip0_light_bank <= pipe0_light_bank;
                    recip0_tex_or_color <= pipe0_tex_or_color;
                    recip0_addr <= pipe0_addr;
                    recip0_z <= pipe0_z;
                    recip0_x <= pipe0_x;
                    recip0_y <= pipe0_y;
                    recip0_uw_q <= pipe0_uw_q;
                    recip0_vw_q <= pipe0_vw_q;
                    recip0_iw_zero <= (pipe0_iw_q == 32'd0);
                    recip0_iw_msb <= pipe0_iw_msb;
                    recip0_iw_norm_q <= pipe0_iw_norm_q;
                    recip0_valid_o <= pipe0_valid_o;
                    recip0_inside_o <= pipe0_inside_o;
                    recip0_ztest_o <= pipe0_ztest_o;
                    recip0_textured_o <= pipe0_textured_o;
                    recip0_alpha_key_o <= pipe0_alpha_key_o;
                    recip0_alpha_o <= pipe0_alpha_o;
                    recip0_fog_o <= pipe0_fog_o;
                    recip0_light_bank_o <= pipe0_light_bank_o;
                    recip0_tex_or_color_o <= pipe0_tex_or_color_o;
                    recip0_addr_o <= pipe0_addr_o;
                    recip0_z_o <= pipe0_z_o;
                    recip0_x_o <= pipe0_x_o;
                    recip0_y_o <= pipe0_y_o;
                    recip0_uw_q_o <= pipe0_uw_q_o;
                    recip0_vw_q_o <= pipe0_vw_q_o;
                    recip0_iw_zero_o <= (pipe0_iw_q_o == 32'd0);
                    recip0_iw_msb_o <= pipe0_iw_msb_o;
                    recip0_iw_norm_q_o <= pipe0_iw_norm_q_o;

                    recip1_valid <= recip0_valid;
                    recip1_inside <= recip0_inside;
                    recip1_ztest <= recip0_ztest;
                    recip1_textured <= recip0_textured;
                    recip1_alpha_key <= recip0_alpha_key;
                    recip1_alpha <= recip0_alpha;
                    recip1_fog <= recip0_fog;
                    recip1_light_bank <= recip0_light_bank;
                    recip1_tex_or_color <= recip0_tex_or_color;
                    recip1_addr <= recip0_addr;
                    recip1_z <= recip0_z;
                    recip1_z_ref <= z_draw_rd_data_e;
                    recip1_dst_rgb565 <= fb_draw_rd_data_e;
                    recip1_x <= recip0_x;
                    recip1_y <= recip0_y;
                    recip1_uw_q <= recip0_uw_q;
                    recip1_vw_q <= recip0_vw_q;
                    recip1_iw_zero <= recip0_iw_zero;
                    recip1_iw_msb <= recip0_iw_msb;
                    recip1_iw_lut_frac <= recip0_iw_lut_frac;
                    recip1_w_norm_lo <= recip_lut[recip0_iw_lut_idx];
                    recip1_w_norm_hi <= recip_lut[recip0_iw_lut_idx + 11'd1];
                    recip1_valid_o <= recip0_valid_o;
                    recip1_inside_o <= recip0_inside_o;
                    recip1_ztest_o <= recip0_ztest_o;
                    recip1_textured_o <= recip0_textured_o;
                    recip1_alpha_key_o <= recip0_alpha_key_o;
                    recip1_alpha_o <= recip0_alpha_o;
                    recip1_fog_o <= recip0_fog_o;
                    recip1_light_bank_o <= recip0_light_bank_o;
                    recip1_tex_or_color_o <= recip0_tex_or_color_o;
                    recip1_addr_o <= recip0_addr_o;
                    recip1_z_o <= recip0_z_o;
                    recip1_z_ref_o <= z_draw_rd_data_o;
                    recip1_dst_rgb565_o <= fb_draw_rd_data_o;
                    recip1_x_o <= recip0_x_o;
                    recip1_y_o <= recip0_y_o;
                    recip1_uw_q_o <= recip0_uw_q_o;
                    recip1_vw_q_o <= recip0_vw_q_o;
                    recip1_iw_zero_o <= recip0_iw_zero_o;
                    recip1_iw_msb_o <= recip0_iw_msb_o;
                    recip1_iw_lut_frac_o <= recip0_iw_lut_frac_o;
                    recip1_w_norm_lo_o <= recip_lut[recip0_iw_lut_idx_o];
                    recip1_w_norm_hi_o <= recip_lut[recip0_iw_lut_idx_o + 11'd1];

                    recip2_valid <= recip1_valid;
                    recip2_inside <= recip1_inside;
                    recip2_ztest <= recip1_ztest;
                    recip2_textured <= recip1_textured;
                    recip2_alpha_key <= recip1_alpha_key;
                    recip2_alpha <= recip1_alpha;
                    recip2_fog <= recip1_fog;
                    recip2_light_bank <= recip1_light_bank;
                    recip2_tex_or_color <= recip1_tex_or_color;
                    recip2_addr <= recip1_addr;
                    recip2_z <= recip1_z;
                    recip2_z_ref <= recip1_z_ref;
                    recip2_dst_rgb565 <= recip1_dst_rgb565;
                    recip2_x <= recip1_x;
                    recip2_y <= recip1_y;
                    recip2_uw_q <= recip1_uw_q;
                    recip2_vw_q <= recip1_vw_q;
                    recip2_iw_zero <= recip1_iw_zero;
                    recip2_iw_msb <= recip1_iw_msb;
                    recip2_w_norm_q <= recip1_w_norm_q;
                    recip2_valid_o <= recip1_valid_o;
                    recip2_inside_o <= recip1_inside_o;
                    recip2_ztest_o <= recip1_ztest_o;
                    recip2_textured_o <= recip1_textured_o;
                    recip2_alpha_key_o <= recip1_alpha_key_o;
                    recip2_alpha_o <= recip1_alpha_o;
                    recip2_fog_o <= recip1_fog_o;
                    recip2_light_bank_o <= recip1_light_bank_o;
                    recip2_tex_or_color_o <= recip1_tex_or_color_o;
                    recip2_addr_o <= recip1_addr_o;
                    recip2_z_o <= recip1_z_o;
                    recip2_z_ref_o <= recip1_z_ref_o;
                    recip2_dst_rgb565_o <= recip1_dst_rgb565_o;
                    recip2_x_o <= recip1_x_o;
                    recip2_y_o <= recip1_y_o;
                    recip2_uw_q_o <= recip1_uw_q_o;
                    recip2_vw_q_o <= recip1_vw_q_o;
                    recip2_iw_zero_o <= recip1_iw_zero_o;
                    recip2_iw_msb_o <= recip1_iw_msb_o;
                    recip2_w_norm_q_o <= recip1_w_norm_q_o;

                    pipe1_valid <= recip2_valid;
                    pipe1_inside <= recip2_inside;
                    pipe1_ztest <= recip2_ztest;
                    pipe1_textured <= recip2_textured;
                    pipe1_alpha_key <= recip2_alpha_key;
                    pipe1_alpha <= recip2_alpha;
                    pipe1_fog <= recip2_fog;
                    pipe1_light_bank <= recip2_light_bank;
                    pipe1_tex_or_color <= recip2_tex_or_color;
                    pipe1_addr <= recip2_addr;
                    pipe1_z <= recip2_z;
                    pipe1_z_ref <= recip2_z_ref;
                    pipe1_dst_rgb565 <= recip2_dst_rgb565;
                    pipe1_x <= recip2_x;
                    pipe1_y <= recip2_y;
                    pipe1_uw_q <= recip2_uw_q;
                    pipe1_vw_q <= recip2_vw_q;
                    pipe1_w_q <= recip2_w_q;
                    pipe1_valid_o <= recip2_valid_o;
                    pipe1_inside_o <= recip2_inside_o;
                    pipe1_ztest_o <= recip2_ztest_o;
                    pipe1_textured_o <= recip2_textured_o;
                    pipe1_alpha_key_o <= recip2_alpha_key_o;
                    pipe1_alpha_o <= recip2_alpha_o;
                    pipe1_fog_o <= recip2_fog_o;
                    pipe1_light_bank_o <= recip2_light_bank_o;
                    pipe1_tex_or_color_o <= recip2_tex_or_color_o;
                    pipe1_addr_o <= recip2_addr_o;
                    pipe1_z_o <= recip2_z_o;
                    pipe1_z_ref_o <= recip2_z_ref_o;
                    pipe1_dst_rgb565_o <= recip2_dst_rgb565_o;
                    pipe1_x_o <= recip2_x_o;
                    pipe1_y_o <= recip2_y_o;
                    pipe1_uw_q_o <= recip2_uw_q_o;
                    pipe1_vw_q_o <= recip2_vw_q_o;
                    pipe1_w_q_o <= recip2_w_q_o;

                    tex0_valid <= pipe1_valid;
                    tex0_inside <= pipe1_inside;
                    tex0_ztest <= pipe1_ztest;
                    tex0_textured <= pipe1_textured;
                    tex0_alpha_key <= pipe1_alpha_key;
                    tex0_alpha <= pipe1_alpha;
                    tex0_fog <= pipe1_fog;
                    tex0_light_bank <= pipe1_light_bank;
                    tex0_tex_or_color <= pipe1_tex_or_color;
                    tex0_addr <= pipe1_addr;
                    tex0_z <= pipe1_z;
                    tex0_z_ref <= pipe1_z_ref;
                    tex0_dst_rgb565 <= pipe1_dst_rgb565;
                    tex0_x <= pipe1_x;
                    tex0_y <= pipe1_y;
                    tex0_w_q <= pipe1_w_q;
                    tex0_u_prod <= pipe1_u_prod;
                    tex0_v_prod <= pipe1_v_prod;
                    tex0_valid_o <= pipe1_valid_o;
                    tex0_inside_o <= pipe1_inside_o;
                    tex0_ztest_o <= pipe1_ztest_o;
                    tex0_textured_o <= pipe1_textured_o;
                    tex0_alpha_key_o <= pipe1_alpha_key_o;
                    tex0_alpha_o <= pipe1_alpha_o;
                    tex0_fog_o <= pipe1_fog_o;
                    tex0_light_bank_o <= pipe1_light_bank_o;
                    tex0_tex_or_color_o <= pipe1_tex_or_color_o;
                    tex0_addr_o <= pipe1_addr_o;
                    tex0_z_o <= pipe1_z_o;
                    tex0_z_ref_o <= pipe1_z_ref_o;
                    tex0_dst_rgb565_o <= pipe1_dst_rgb565_o;
                    tex0_x_o <= pipe1_x_o;
                    tex0_y_o <= pipe1_y_o;
                    tex0_w_q_o <= pipe1_w_q_o;
                    tex0_u_prod_o <= pipe1_u_prod_o;
                    tex0_v_prod_o <= pipe1_v_prod_o;

                    pipe2_valid <= tex0_valid;
                    pipe2_inside <= tex0_inside;
                    pipe2_ztest <= tex0_ztest;
                    pipe2_textured <= tex0_textured;
                    pipe2_alpha_key <= tex0_alpha_key;
                    pipe2_alpha <= tex0_alpha;
                    pipe2_fog <= tex0_fog;
                    pipe2_light_bank <= tex0_light_bank;
                    pipe2_tex_or_color <= tex0_tex_or_color;
                    pipe2_addr <= tex0_addr;
                    pipe2_z <= tex0_z;
                    pipe2_z_ref <= tex0_z_ref;
                    pipe2_dst_rgb565 <= tex0_dst_rgb565;
                    pipe2_x <= tex0_x;
                    pipe2_y <= tex0_y;
                    pipe2_w_q <= tex0_w_q;
                    pipe2_tex_addr <= tex0_tex_addr;
                    pipe2_valid_o <= tex0_valid_o;
                    pipe2_inside_o <= tex0_inside_o;
                    pipe2_ztest_o <= tex0_ztest_o;
                    pipe2_textured_o <= tex0_textured_o;
                    pipe2_alpha_key_o <= tex0_alpha_key_o;
                    pipe2_alpha_o <= tex0_alpha_o;
                    pipe2_fog_o <= tex0_fog_o;
                    pipe2_light_bank_o <= tex0_light_bank_o;
                    pipe2_tex_or_color_o <= tex0_tex_or_color_o;
                    pipe2_addr_o <= tex0_addr_o;
                    pipe2_z_o <= tex0_z_o;
                    pipe2_z_ref_o <= tex0_z_ref_o;
                    pipe2_dst_rgb565_o <= tex0_dst_rgb565_o;
                    pipe2_x_o <= tex0_x_o;
                    pipe2_y_o <= tex0_y_o;
                    pipe2_w_q_o <= tex0_w_q_o;
                    pipe2_tex_addr_o <= tex0_tex_addr_o;

                    draw_pipe_valid <= pipe2_valid;
                    draw_pipe_inside <= pipe2_inside;
                    draw_pipe_ztest <= pipe2_ztest;
                    draw_pipe_textured <= pipe2_textured;
                    draw_pipe_alpha_key <= pipe2_alpha_key;
                    draw_pipe_alpha <= pipe2_alpha;
                    draw_pipe_fog <= pipe2_fog;
                    draw_pipe_light_bank <= pipe2_light_bank;
                    draw_pipe_tex_or_color <= pipe2_tex_or_color;
                    draw_pipe_addr <= pipe2_addr;
                    draw_pipe_z <= pipe2_z;
                    draw_pipe_z_ref <= pipe2_z_ref;
                    draw_pipe_dst_rgb565 <= pipe2_dst_rgb565;
                    draw_pipe_x <= pipe2_x;
                    draw_pipe_y <= pipe2_y;
                    draw_pipe_w_q <= pipe2_w_q;
                    draw_pipe_valid_o <= pipe2_valid_o;
                    draw_pipe_inside_o <= pipe2_inside_o;
                    draw_pipe_ztest_o <= pipe2_ztest_o;
                    draw_pipe_textured_o <= pipe2_textured_o;
                    draw_pipe_alpha_key_o <= pipe2_alpha_key_o;
                    draw_pipe_alpha_o <= pipe2_alpha_o;
                    draw_pipe_fog_o <= pipe2_fog_o;
                    draw_pipe_light_bank_o <= pipe2_light_bank_o;
                    draw_pipe_tex_or_color_o <= pipe2_tex_or_color_o;
                    draw_pipe_addr_o <= pipe2_addr_o;
                    draw_pipe_z_o <= pipe2_z_o;
                    draw_pipe_z_ref_o <= pipe2_z_ref_o;
                    draw_pipe_dst_rgb565_o <= pipe2_dst_rgb565_o;
                    draw_pipe_x_o <= pipe2_x_o;
                    draw_pipe_y_o <= pipe2_y_o;
                    draw_pipe_w_q_o <= pipe2_w_q_o;

                    if (state == ST_DRAW && draw_cache_hit) begin
                        pipe0_valid <= 1'b1;
                        pipe0_inside <= draw_inside_lane0;
                        pipe0_ztest <= draw_flags[FLAG_ZTEST_BIT];
                        pipe0_textured <= draw_flags[FLAG_TEX_BIT];
                        pipe0_alpha_key <= draw_flags[FLAG_ALPHA_KEY_BIT];
                        pipe0_alpha <= draw_flags[FLAG_ALPHA_MSB:FLAG_ALPHA_LSB];
                        pipe0_fog <= draw_flags[FLAG_FOG_BIT];
                        pipe0_light_bank <= draw_flags[FLAG_LIGHT_MSB:FLAG_LIGHT_LSB];
                        pipe0_tex_or_color <= draw_tex_or_color;
                        pipe0_addr <= draw_addr;
                        pipe0_z <= draw_z_value;
                        pipe0_x <= draw_x_cur;
                        pipe0_y <= draw_y_cur;
                        pipe0_uw_q <= draw_uw_q;
                        pipe0_vw_q <= draw_vw_q;
                        pipe0_iw_q <= draw_iw_q;
                        pipe0_valid_o <= 1'b1;
                        pipe0_inside_o <= draw_inside_lane1;
                        pipe0_ztest_o <= draw_flags[FLAG_ZTEST_BIT];
                        pipe0_textured_o <= draw_flags[FLAG_TEX_BIT];
                        pipe0_alpha_key_o <= draw_flags[FLAG_ALPHA_KEY_BIT];
                        pipe0_alpha_o <= draw_flags[FLAG_ALPHA_MSB:FLAG_ALPHA_LSB];
                        pipe0_fog_o <= draw_flags[FLAG_FOG_BIT];
                        pipe0_light_bank_o <= draw_flags[FLAG_LIGHT_MSB:FLAG_LIGHT_LSB];
                        pipe0_tex_or_color_o <= draw_tex_or_color;
                        pipe0_addr_o <= draw_addr_o;
                        pipe0_z_o <= draw_z_value_o;
                        pipe0_x_o <= draw_x_next;
                        pipe0_y_o <= draw_y_cur;
                        pipe0_uw_q_o <= draw_uw_q_o;
                        pipe0_vw_q_o <= draw_vw_q_o;
                        pipe0_iw_q_o <= draw_iw_q_o;

                        /* Track inside-to-outside transition for early row exit */
                        if (draw_pair_edge_inside)
                            draw_row_inside <= 1'b1;

                        /* Early scanline exit: if we were inside the quad on
                         * this row and now we're outside, all remaining pixels
                         * on this row are also outside (convex quad property).
                         * Skip directly to the next row. */
                        if (draw_pair_last || draw_pair_exited) begin
                            if (draw_y_cur == draw_y_max) begin
                                state <= ST_DRAW_FLUSH;
                                draw_flush_count <= DRAW_FLUSH_CYCLES;
                            end else begin
                                draw_row_base <= draw_row_base + 16'd640;
                                draw_x_cur <= draw_x_start_even;
                                draw_y_cur <= draw_y_cur + 9'd1;
                                draw_row_inside <= 1'b0;
                                edge_row_val[0] <= edge_row_val[0] + edge_B[0];
                                edge_cur_val[0] <= edge_row_val[0] + edge_B[0];
                                edge_row_val[1] <= edge_row_val[1] + edge_B[1];
                                edge_cur_val[1] <= edge_row_val[1] + edge_B[1];
                                edge_row_val[2] <= edge_row_val[2] + edge_B[2];
                                edge_cur_val[2] <= edge_row_val[2] + edge_B[2];
                                edge_row_val[3] <= edge_row_val[3] + edge_B[3];
                                edge_cur_val[3] <= edge_row_val[3] + edge_B[3];
                                z_row_val <= z_row_val + draw_dz_dy;
                                z_cur_val <= z_row_val + draw_dz_dy;
                                uw_row_val <= uw_row_val + draw_uw_dy;
                                uw_cur_val <= uw_row_val + draw_uw_dy;
                                vw_row_val <= vw_row_val + draw_vw_dy;
                                vw_cur_val <= vw_row_val + draw_vw_dy;
                                iw_row_val <= iw_row_val + draw_iw_dy;
                                iw_cur_val <= iw_row_val + draw_iw_dy;
                            end
                        end else begin
                            draw_x_cur <= draw_x_cur + 10'd2;
                            edge_cur_val[0] <= edge_cur_val[0] + edge_A[0] + edge_A[0];
                            edge_cur_val[1] <= edge_cur_val[1] + edge_A[1] + edge_A[1];
                            edge_cur_val[2] <= edge_cur_val[2] + edge_A[2] + edge_A[2];
                            edge_cur_val[3] <= edge_cur_val[3] + edge_A[3] + edge_A[3];
                            z_cur_val <= z_cur_val + draw_dz_dx_ext + draw_dz_dx_ext;
                            uw_cur_val <= uw_cur_val + draw_uw_dx_ext + draw_uw_dx_ext;
                            vw_cur_val <= vw_cur_val + draw_vw_dx_ext + draw_vw_dx_ext;
                            iw_cur_val <= iw_cur_val + draw_iw_dx_ext + draw_iw_dx_ext;
                        end
                    end else if (state == ST_DRAW) begin
                        pipe0_valid <= 1'b0;
                        pipe0_valid_o <= 1'b0;
                        /*
                         * Software has already binned this descriptor into each
                         * overlapping band pass. Pixels outside the resident
                         * band are ignored here and will be drawn during their
                         * own BEGIN_BAND/END_BAND pass.
                         */
                        /* Early scanline exit for cache-miss path too */
                        if (draw_pair_edge_inside)
                            draw_row_inside <= 1'b1;

                        if (draw_pair_last || draw_pair_exited) begin
                            if (draw_y_cur == draw_y_max) begin
                                state <= ST_DRAW_FLUSH;
                                draw_flush_count <= DRAW_FLUSH_CYCLES;
                            end else begin
                                draw_row_base <= draw_row_base + 16'd640;
                                draw_x_cur <= draw_x_start_even;
                                draw_y_cur <= draw_y_cur + 9'd1;
                                draw_row_inside <= 1'b0;
                                edge_row_val[0] <= edge_row_val[0] + edge_B[0];
                                edge_cur_val[0] <= edge_row_val[0] + edge_B[0];
                                edge_row_val[1] <= edge_row_val[1] + edge_B[1];
                                edge_cur_val[1] <= edge_row_val[1] + edge_B[1];
                                edge_row_val[2] <= edge_row_val[2] + edge_B[2];
                                edge_cur_val[2] <= edge_row_val[2] + edge_B[2];
                                edge_row_val[3] <= edge_row_val[3] + edge_B[3];
                                edge_cur_val[3] <= edge_row_val[3] + edge_B[3];
                                z_row_val <= z_row_val + draw_dz_dy;
                                z_cur_val <= z_row_val + draw_dz_dy;
                                uw_row_val <= uw_row_val + draw_uw_dy;
                                uw_cur_val <= uw_row_val + draw_uw_dy;
                                vw_row_val <= vw_row_val + draw_vw_dy;
                                vw_cur_val <= vw_row_val + draw_vw_dy;
                                iw_row_val <= iw_row_val + draw_iw_dy;
                                iw_cur_val <= iw_row_val + draw_iw_dy;
                            end
                        end else begin
                            draw_x_cur <= draw_x_cur + 10'd2;
                            edge_cur_val[0] <= edge_cur_val[0] + edge_A[0] + edge_A[0];
                            edge_cur_val[1] <= edge_cur_val[1] + edge_A[1] + edge_A[1];
                            edge_cur_val[2] <= edge_cur_val[2] + edge_A[2] + edge_A[2];
                            edge_cur_val[3] <= edge_cur_val[3] + edge_A[3] + edge_A[3];
                            z_cur_val <= z_cur_val + draw_dz_dx_ext + draw_dz_dx_ext;
                            uw_cur_val <= uw_cur_val + draw_uw_dx_ext + draw_uw_dx_ext;
                            vw_cur_val <= vw_cur_val + draw_vw_dx_ext + draw_vw_dx_ext;
                            iw_cur_val <= iw_cur_val + draw_iw_dx_ext + draw_iw_dx_ext;
                        end
                    end else begin
                        pipe0_valid <= 1'b0;
                        pipe0_inside <= 1'b0;
                        pipe0_ztest <= 1'b0;
                        pipe0_textured <= 1'b0;
                        pipe0_alpha_key <= 1'b0;
                        pipe0_alpha <= 2'd0;
                        pipe0_fog <= 1'b0;
                        pipe0_light_bank <= 2'd0;
                        pipe0_tex_or_color <= 8'd0;
                        pipe0_addr <= 16'd0;
                        pipe0_z <= 16'd0;
                        pipe0_x <= 10'd0;
                        pipe0_y <= 9'd0;
                        pipe0_uw_q <= 32'sd0;
                        pipe0_vw_q <= 32'sd0;
                        pipe0_iw_q <= 32'd0;
                        pipe0_valid_o <= 1'b0;
                        pipe0_inside_o <= 1'b0;
                        pipe0_ztest_o <= 1'b0;
                        pipe0_textured_o <= 1'b0;
                        pipe0_alpha_key_o <= 1'b0;
                        pipe0_alpha_o <= 2'd0;
                        pipe0_fog_o <= 1'b0;
                        pipe0_light_bank_o <= 2'd0;
                        pipe0_tex_or_color_o <= 8'd0;
                        pipe0_addr_o <= 16'd0;
                        pipe0_z_o <= 16'd0;
                        pipe0_x_o <= 10'd0;
                        pipe0_y_o <= 9'd0;
                        pipe0_uw_q_o <= 32'sd0;
                        pipe0_vw_q_o <= 32'sd0;
                        pipe0_iw_q_o <= 32'd0;

                        if (draw_flush_count == 4'd1) begin
                            draw_flush_count <= 4'd0;
                            state <= ST_IDLE;
                        end else begin
                            draw_flush_count <= draw_flush_count - 4'd1;
                        end
                    end
                end

                ST_CACHE_EVICT: begin
                    cache_words_issued <= 16'd0;
                    cache_words_done <= 16'd0;
                    cache_maint_addr <= 16'd0;
                    cache_fetch_inflight <= 1'b0;
                    cache_word_pending_valid <= 1'b0;
                    cache_drain_count <= 8'd0;
                    cache_pixels_total <= band_pixel_count(cache_band_index);
                    if (cache_valid && cache_dirty) begin
                        sdram_wr_addr_cfg <= copy_target_base_words +
                                             band_word_offset(cache_band_index);
                        sdram_wr_max_addr_cfg <= copy_target_base_words +
                                                 band_word_offset(cache_band_index) +
                                                 band_word_count(cache_band_index);
                        cache_flush_load_pending <= 1'b1;
                        state <= ST_CACHE_FLUSH_COLOR;
                    end else begin
                        state <= ST_CACHE_SELECT_FILL;
                    end
                end

                ST_CACHE_FLUSH_COLOR: begin
                    if (cache_flush_load_pending && !scanout_read_load_req) begin
                        sdram_wr_load_pulse <= 1'b1;
                        cache_flush_load_pending <= 1'b0;
                    end

                    if ((cache_words_done == cache_pixels_total) &&
                        !cache_word_pending_valid &&
                        !cache_fetch_inflight &&
                        (sdram_wr_use[8:0] == 9'd0)) begin
                        if (cache_drain_count == COPY_DRAIN_CYCLES) begin
                            cache_words_issued <= 16'd0;
                            cache_words_done <= 16'd0;
                            cache_maint_addr <= 16'd0;
                            cache_fetch_inflight <= 1'b0;
                            cache_word_pending_valid <= 1'b0;
                            cache_band_valid[cache_band_index] <= 1'b1;
                            cache_dirty <= 1'b0;
                            cache_draw_dirty <= 1'b0;
                            cache_valid <= 1'b0;
                            cache_drain_count <= 8'd0;
                            state <= ST_IDLE;
                        end else begin
                            cache_drain_count <= cache_drain_count + 8'd1;
                        end
                    end else begin
                        cache_drain_count <= 8'd0;
                    end
                end

                ST_CACHE_FLUSH_Z: begin
                    if ((cache_words_done == cache_pixels_total) &&
                        !cache_word_pending_valid &&
                        !cache_fetch_inflight &&
                        (sdram_wr_use[8:0] == 9'd0)) begin
                        if (cache_drain_count == COPY_DRAIN_CYCLES) begin
                            cache_band_valid[cache_band_index] <= 1'b1;
                            cache_dirty <= 1'b0;
                            cache_draw_dirty <= 1'b0;
                            cache_valid <= 1'b0;
                            cache_drain_count <= 8'd0;
                            if (cache_final_flush) begin
                                copy_complete_pending <= 1'b1;
                                cache_final_flush <= 1'b0;
                                state <= ST_IDLE;
                            end else begin
                                state <= ST_CACHE_SELECT_FILL;
                            end
                        end else begin
                            cache_drain_count <= cache_drain_count + 8'd1;
                        end
                    end else begin
                        cache_drain_count <= 8'd0;
                    end
                end

                ST_CACHE_SELECT_FILL: begin
                    cache_band_index <= cache_target_band;
                    cache_pixels_total <= band_pixel_count(cache_target_band);
                    cache_words_done <= 16'd0;
                    if (cache_band_valid[cache_target_band] && cache_read_start_ok) begin
                        sdram_rd_addr_cfg <= copy_target_base_words +
                                             band_word_offset(cache_target_band);
                        sdram_rd_max_addr_cfg <= copy_target_base_words +
                                                 band_word_offset(cache_target_band) +
                                                 band_word_count(cache_target_band);
                        sdram_rd_load_pulse <= 1'b1;
                        sdram_rd_load_stretch_req <= 1'b1;
                        state <= ST_CACHE_LOAD_COLOR;
                    end else if (cache_band_valid[cache_target_band]) begin
                        state <= ST_CACHE_SELECT_FILL;
                    end else begin
                        cache_init_x <= 10'd0;
                        cache_init_sky_row_count <= sky_clear_start_row_count(cache_target_band);
                        cache_init_sky_palette <= sky_clear_start_palette(cache_target_band);
                        state <= ST_CACHE_INIT;
                    end
                end

                ST_CACHE_INIT: begin
                    if (cache_maint_addr == cache_pixels_total - 16'd1) begin
                        cache_valid <= 1'b1;
                        cache_dirty <= sky_gradient_clear_enabled;
                        cache_draw_dirty <= 1'b0;
                        cache_band_index <= cache_target_band;
                        cache_maint_addr <= 16'd0;
                        cache_init_x <= 10'd0;
                        cache_resume_draw <= 1'b0;
                        state <= ST_IDLE;
                    end else begin
                        cache_maint_addr <= cache_maint_addr + 16'd1;
                        if (cache_init_x == 10'd639) begin
                            cache_init_x <= 10'd0;
                            if (cache_init_sky_row_count == 5'd19) begin
                                cache_init_sky_row_count <= 5'd0;
                                if (cache_init_sky_palette != PAL_SKY_GRADIENT_LAST)
                                    cache_init_sky_palette <= cache_init_sky_palette + 8'd1;
                            end else begin
                                cache_init_sky_row_count <= cache_init_sky_row_count + 5'd1;
                            end
                        end else begin
                            cache_init_x <= cache_init_x + 10'd1;
                        end
                    end
                end

                ST_CACHE_LOAD_COLOR: begin
                    if (cache_rd_capture &&
                        (cache_words_done == cache_pixels_total - 16'd1)) begin
                        cache_words_done <= 16'd0;
                        cache_maint_addr <= 16'd0;
                        cache_drain_count <= 8'd0;
                        state <= ST_CACHE_DRAIN_COLOR;
                    end
                end

                /*
                 * Drain residual words from the SDRAM RD FIFO. The Sdram
                 * controller auto-bursts in 64-word chunks, so the last
                 * burst over-fetches up to 63 words past cache_pixels_total.
                 * Pop until rd_empty has been stable for COPY_DRAIN_CYCLES
                 * before issuing the next RD_LOAD. Without this, those
                 * residuals sit in the FIFO and the next scan-fill burst
                 * reads them as its first chunk → 64-pixel left-edge streak.
                 */
                ST_CACHE_DRAIN_COLOR: begin
                    if (sdram_rd_empty) begin
                        if (cache_drain_count == COPY_DRAIN_CYCLES) begin
                            cache_drain_count <= 8'd0;
                            state <= ST_CACHE_START_LOAD_Z;
                        end else begin
                            cache_drain_count <= cache_drain_count + 8'd1;
                        end
                    end else begin
                        cache_drain_count <= 8'd0;
                    end
                end

                ST_CACHE_START_LOAD_Z: begin
                    if (cache_read_start_ok) begin
                        sdram_rd_addr_cfg <= extmem_z_base_words +
                                             band_word_offset(cache_target_band);
                        sdram_rd_max_addr_cfg <= extmem_z_base_words +
                                                 band_word_offset(cache_target_band) +
                                                 band_word_count(cache_target_band);
                        sdram_rd_load_pulse <= 1'b1;
                        sdram_rd_load_stretch_req <= 1'b1;
                        state <= ST_CACHE_LOAD_Z;
                    end
                end

                ST_CACHE_LOAD_Z: begin
                    if (cache_rd_capture &&
                        (cache_words_done == cache_pixels_total - 16'd1)) begin
                        cache_valid <= 1'b1;
                        cache_dirty <= 1'b0;
                        cache_draw_dirty <= 1'b0;
                        cache_band_index <= cache_target_band;
                        cache_words_done <= 16'd0;
                        cache_maint_addr <= 16'd0;
                        cache_drain_count <= 8'd0;
                        state <= ST_CACHE_DRAIN_Z;
                    end
                end

                /* See ST_CACHE_DRAIN_COLOR comment. */
                ST_CACHE_DRAIN_Z: begin
                    if (sdram_rd_empty) begin
                        if (cache_drain_count == COPY_DRAIN_CYCLES) begin
                            cache_drain_count <= 8'd0;
                            cache_resume_draw <= 1'b0;
                            state <= cache_resume_draw ? ST_DRAW : ST_IDLE;
                        end else begin
                            cache_drain_count <= cache_drain_count + 8'd1;
                        end
                    end else begin
                        cache_drain_count <= 8'd0;
                    end
                end

                default: state <= ST_IDLE;
            endcase

            /*
             * CLR is a frame-level abort/restart request from the driver. Do
             * not wait for ST_IDLE: cache maintenance can legitimately take a
             * long time at 640x480, and if a previous frame wedges we need the
             * next CLEAR_FRAME ioctl to recover the engine instead of timing
             * out behind stale BUSY.
             */
            if (ctrl_clear_write) begin
                state <= ST_IDLE;
                clear_pending <= 1'b1;
                ctrl_flp_pending <= 1'b0;
                copy_complete_pending <= 1'b0;
                copy_target_sel <= ~display_sel;
                cache_valid <= 1'b0;
                cache_dirty <= 1'b0;
                cache_draw_dirty <= 1'b0;
                cache_band_valid <= 8'h00;
                band_begin_pending <= 1'b0;
                band_flush_pending <= 1'b0;
                cache_resume_draw <= 1'b0;
                cache_final_flush <= 1'b0;
                draw_cache_sel <= 1'b0;
                /*
                 * Do not reset background-flush state here. CLEAR_FRAME
                 * fires at the start of every software frame and on fast
                 * sky/ground views it races the tail end of the previous
                 * frame's band flush. Killing flush_active mid-stream
                 * leaves partial data in the SDRAM framebuffer, which
                 * scanout displays as a flash. The flush writes to the
                 * old back buffer and completes harmlessly on its own;
                 * band_begin_cache_available correctly stalls new bands
                 * if the flush still owns a local cache.
                 */
                cache_words_issued <= 16'd0;
                cache_words_done <= 16'd0;
                cache_maint_addr <= 16'd0;
                cache_init_x <= 10'd0;
                cache_init_sky_row_count <= 5'd0;
                cache_init_sky_palette <= PAL_SKY_GRADIENT_BASE;
                cache_rd_capture <= 1'b0;
                cache_fetch_inflight <= 1'b0;
                cache_flush_load_pending <= 1'b0;
                cache_word_pending_valid <= 1'b0;
                sdram_wr_load_pulse <= 1'b0;
                scan_late_count <= 16'd0;
                /*
                 * Do not reset the scanout RD pipeline here. CLEAR_FRAME is
                 * issued at the start of every software frame, and with the
                 * 30 FPS cap it can land in the middle of active display.
                 * Dropping scan_rd_capture/load/hold state mid-line corrupts
                 * the line-buffer fill and shows up as sky/ground flashing.
                 */
                fifo_wr_ptr <= 11'd0;
                fifo_rd_ptr <= 11'd0;
                fifo_count <= 12'd0;
                fetch_count <= 6'd0;
                draw_flush_count <= 4'd0;
                pipe0_valid <= 1'b0;
                recip0_valid <= 1'b0;
                recip1_valid <= 1'b0;
                recip2_valid <= 1'b0;
                pipe1_valid <= 1'b0;
                tex0_valid <= 1'b0;
                pipe2_valid <= 1'b0;
                draw_pipe_valid <= 1'b0;
                pal_rd_valid <= 1'b0;
                plr_valid <= 1'b0;
                fog0_valid <= 1'b0;
                fog1_valid <= 1'b0;
                commit_valid <= 1'b0;
                pipe0_valid_o <= 1'b0;
                recip0_valid_o <= 1'b0;
                recip1_valid_o <= 1'b0;
                recip2_valid_o <= 1'b0;
                pipe1_valid_o <= 1'b0;
                tex0_valid_o <= 1'b0;
                pipe2_valid_o <= 1'b0;
                draw_pipe_valid_o <= 1'b0;
                pal_rd_valid_o <= 1'b0;
                plr_valid_o <= 1'b0;
                fog0_valid_o <= 1'b0;
                fog1_valid_o <= 1'b0;
                commit_valid_o <= 1'b0;
            end

            extmem_dma_status <= {
                scan_late_count,
                cache_band_index,
                display_sel,
                copy_target_sel,
                copy_complete_pending,
                cache_maint_state,
                flush_active,
                scan_fill_active,
                display_valid,
                sdram_ready,
                scan_active_bank[0],
                scan_fill_bank[0],
                scan_line2_ready,
                scan_line1_ready,
                scan_line0_ready
            };
        end
    end

    /*
     * Per-frame perf counters — accumulate while a frame is in flight,
     * reset on the FLIP write that ends the frame. Software reads them
     * just before issuing FLIP, so the values reflect the just-completed
     * frame's activity. Kept in a separate always_ff so the main FSM
     * stays untouched and these can be ifdef'd out cleanly later.
     *
     * Note: counters CAN overlap. Background flush runs in parallel with
     * the rasterizer (perf_flush_active + perf_draw_active in the same
     * cycle) and ST_CACHE_INIT runs on the opposite cache while a flush
     * is in flight (perf_init + perf_flush_active). Sums therefore exceed
     * wall-clock cycles by design.
     */
    wire perf_flip_write = wr && (address == ADDR_CONTROL) && writedata[1];
    wire perf_in_draw    = (state == ST_DRAW) || (state == ST_DRAW_FLUSH);
    wire perf_draw_commit = commit_valid || commit_valid_o;
    wire perf_in_load    = (state == ST_CACHE_LOAD_COLOR) ||
                           (state == ST_CACHE_LOAD_Z) ||
                           (state == ST_CACHE_DRAIN_COLOR) ||
                           (state == ST_CACHE_DRAIN_Z) ||
                           (state == ST_CACHE_START_LOAD_Z);
    wire perf_flush_push = bg_flush_wr_push || main_flush_wr_push;

    always_ff @(posedge clk) begin
        if (reset || perf_flip_write) begin
            perf_draw_active  <= 32'd0;
            perf_draw_idle    <= 32'd0;
            perf_flush_active <= 32'd0;
            perf_flush_stall  <= 32'd0;
            perf_init         <= 32'd0;
            perf_load         <= 32'd0;
        end else begin
            if (perf_in_draw && perf_draw_commit)
                perf_draw_active <= perf_draw_active + 32'd1;
            if (perf_in_draw && !perf_draw_commit)
                perf_draw_idle <= perf_draw_idle + 32'd1;
            if (flush_active &&  perf_flush_push)
                perf_flush_active <= perf_flush_active + 32'd1;
            if (flush_active && !perf_flush_push)
                perf_flush_stall <= perf_flush_stall + 32'd1;
            if (state == ST_CACHE_INIT)
                perf_init <= perf_init + 32'd1;
            if (perf_in_load)
                perf_load <= perf_load + 32'd1;
        end
    end

    always_comb begin
        case (address)
            ADDR_CONTROL : readdata = control_word;
            ADDR_STATUS  : readdata = status_word;
            ADDR_FRAMECNT: readdata = frame_count;
            ADDR_PAL_ADDR: readdata = {24'h0, pal_addr};
            ADDR_FOG_RANGE: readdata = {fog_end_dist, fog_start_dist};
            ADDR_FOG_CTRL: readdata = {fog_inv_proj_sq, 7'h0, fog_enable, fog_color};
            ADDR_EXTMEM_CTRL: readdata = extmem_ctrl;
            ADDR_EXTMEM_FRONT: readdata = extmem_front_base;
            ADDR_EXTMEM_BACK: readdata = extmem_back_base;
            ADDR_EXTMEM_STRIDE: readdata = extmem_stride_bytes;
            ADDR_EXTMEM_TILE: readdata = extmem_tile_cfg;
            ADDR_EXTMEM_STAT: readdata = extmem_dma_status;
            ADDR_BAND_INDEX: readdata = {29'h0, band_index_cfg};
            ADDR_BAND_CTRL: readdata = {30'h0, band_flush_pending, band_begin_pending};
            ADDR_PERF_DRAW_ACT : readdata = perf_draw_active;
            ADDR_PERF_DRAW_IDLE: readdata = perf_draw_idle;
            ADDR_PERF_FLUSH_ACT: readdata = perf_flush_active;
            ADDR_PERF_FLUSH_STL: readdata = perf_flush_stall;
            ADDR_PERF_INIT     : readdata = perf_init;
            ADDR_PERF_LOAD     : readdata = perf_load;
            default      : readdata = 32'h0;  /* palette readback not needed by driver */
        endcase
    end

    always_comb begin
        if (scan_visible_r) begin
            VGA_R = pixel_rgb[23:16];
            VGA_G = pixel_rgb[15:8];
            VGA_B = pixel_rgb[7:0];
        end else begin
            VGA_R = 8'h00;
            VGA_G = 8'h00;
            VGA_B = 8'h00;
        end
    end

endmodule
