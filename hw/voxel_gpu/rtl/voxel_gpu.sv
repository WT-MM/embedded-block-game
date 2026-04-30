// voxel_gpu.sv — SDRAM-backed display path with RGB565 external frames.
//
// The rasterizer renders into a single 320x240 RGB565 BRAM backbuffer plus a
// BRAM z-buffer. Palette entries are still the source-color ABI, but resolved
// pixels are stored as true color so translucent quads can alpha blend against
// the existing destination pixel.
// FLIP no longer swaps two BRAM framebuffers. Instead:
//   * the finished RGB565 BRAM backbuffer is copied into the inactive SDRAM
//     frame,
//   * scanout reads the active SDRAM frame through small line buffers, and
//   * the visible SDRAM frame only changes on vsync once the copy completes.
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
    localparam logic [12:0] ADDR_FIFO_LO  = 13'h400;  // 0x1000
    localparam logic [12:0] ADDR_FIFO_HI  = 13'hC00;  // 0x3000 (exclusive)

    localparam int FB_WIDTH       = 320;
    localparam int FB_HEIGHT      = 240;
    localparam int FB_PIXELS      = FB_WIDTH * FB_HEIGHT;
    localparam int FIFO_DEPTH     = 2048;
    localparam int BASE_QUAD_WORDS = 16;
    localparam int UV_QUAD_WORDS   = 16;
    localparam int MAX_DESC_WORDS  = BASE_QUAD_WORDS + UV_QUAD_WORDS;
    localparam int TEXTURE_BYTES   = 64 * 16 * 16;
    localparam int FB_WORDS        = FB_PIXELS;
    localparam int LINE_WORDS      = FB_WIDTH;
    localparam int COPY_BURST_WORDS = 64;
    localparam int READ_BURST_WORDS = 64;
    localparam logic [8:0] COPY_WR_FIFO_HIGH_WATER = 9'd64;
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
    localparam logic [24:0] FB_WORDS_25 = 25'd76800;
    localparam logic [24:0] LINE_WORDS_25 = 25'd320;
    localparam logic [24:0] READ_BURST_WORDS_25 = 25'd64;
    localparam logic [8:0]  LINE_WORDS_9 = 9'd320;
    localparam logic [8:0]  COPY_BURST_WORDS_9 = 9'd64;
    localparam logic [8:0]  READ_BURST_WORDS_9 = 9'd64;
    localparam logic [31:0] DEFAULT_EXTMEM_CTRL = 32'h0000_000B;
    localparam logic [31:0] DEFAULT_EXTMEM_FRONT_BASE = 32'd0;
    localparam logic [31:0] DEFAULT_EXTMEM_BACK_BASE = 32'd153600;
    localparam logic [31:0] DEFAULT_EXTMEM_STRIDE = 32'd640;
    localparam int SDRAM_POWERUP_HOLD_CYCLES = 200000;
    localparam int SDRAM_INIT_WAIT_CYCLES = 32000;
    localparam int FLAG_TEX_BIT        = 0;
    localparam int FLAG_ZTEST_BIT      = 1;
    localparam int FLAG_ALPHA_KEY_BIT  = 2;
    localparam int FLAG_FOG_BIT        = 3;
    localparam int FLAG_LIGHT_LSB      = 4;
    localparam int FLAG_LIGHT_MSB      = 5;
    localparam int FLAG_ALPHA_LSB      = 6;
    localparam int FLAG_ALPHA_MSB      = 7;

    typedef enum logic [3:0] {
        ST_IDLE       = 4'd0,
        ST_CLEAR      = 4'd1,
        ST_FETCH      = 4'd2,
        ST_DRAW       = 4'd3,
        ST_DRAW_FLUSH = 4'd4,
        ST_COPY       = 4'd5
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
    logic        copy_target_sel;
    logic        copy_complete_pending;

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

    logic [16:0] clear_addr;
    logic [16:0] draw_row_base;
    logic  [9:0] draw_x_min, draw_x_max, draw_x_cur;
    logic  [8:0] draw_y_min;
    logic  [8:0] draw_y_max, draw_y_cur;
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
    logic        pipe0_valid;
    logic        pipe0_inside;
    logic        pipe0_ztest;
    logic        pipe0_textured;
    logic        pipe0_alpha_key;
    logic  [1:0] pipe0_alpha;
    logic        pipe0_fog;
    logic  [1:0] pipe0_light_bank;
    logic  [7:0] pipe0_tex_or_color;
    logic [16:0] pipe0_addr;
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
    logic [16:0] recip0_addr;
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
    logic [16:0] recip1_addr;
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
    logic [16:0] recip2_addr;
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
    logic [16:0] pipe1_addr;
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
    logic [16:0] tex0_addr;
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
    logic [16:0] pipe2_addr;
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
    logic [16:0] draw_pipe_addr;
    logic [15:0] draw_pipe_z;
    logic [15:0] draw_pipe_z_ref;
    logic [15:0] draw_pipe_dst_rgb565;
    logic  [9:0] draw_pipe_x;
    logic  [8:0] draw_pipe_y;
    logic [31:0] draw_pipe_w_q;

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
    logic [16:0] pal_rd_addr;
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
    logic [16:0] plr_addr;
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
    logic [16:0] fog0_addr;
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
    logic [16:0] fog1_addr;
    logic [15:0] fog1_z;
    logic [15:0] fog1_src_rgb565;
    logic [15:0] fog1_dst_rgb565;
    logic [15:0] fog1_fog_rgb565;
    logic [15:0] fog1_radial_q8_8;
    logic        commit_valid;
    logic        commit_pass;
    logic        commit_ztest;
    logic [16:0] commit_addr;
    logic [15:0] commit_z;
    logic [15:0] commit_color;
    // tex_rd_data is driven combinationally by voxel_texture_rom's
    // registered output. The ROM takes pipe2_tex_addr on cycle T and
    // presents mem[pipe2_tex_addr[T]] on cycle T+1, which is the same
    // 1-cycle latency the draw_pipe stage expects (see the instance
    // below and the voxel_texture_rom module header for rationale).
    wire   [7:0] tex_rd_data;

    logic [10:0] hcount;
    logic  [9:0] vcount;

    logic        scan_visible_now;
    logic [15:0] scan_rgb565_r;
    logic        scan_visible_r;
    logic [16:0] draw_addr;
    logic [16:0] fb_back_rd_addr;
    logic [16:0] copy_fb_rd_addr;
    logic [15:0] fb_back_rd_data;
    logic [16:0] fb_wr_addr;
    logic [15:0] fb_wr_data;
    logic        fb_back_wr_en;
    logic [16:0] z_rd_addr;
    logic [15:0] z_rd_data;
    logic [16:0] z_wr_addr;
    logic [15:0] z_wr_data;
    logic        z_wr_en;

    (* ramstyle = "MLAB, no_rw_check" *) logic [15:0] scan_linebuf0 [0:LINE_WORDS-1];
    (* ramstyle = "MLAB, no_rw_check" *) logic [15:0] scan_linebuf1 [0:LINE_WORDS-1];
    logic        scan_line0_ready;
    logic        scan_line1_ready;
    logic  [8:0] scan_line0_row;
    logic  [8:0] scan_line1_row;
    logic [24:0] scan_line0_base_words;
    logic [24:0] scan_line1_base_words;
    logic        scan_active_bank;
    logic        scan_active_valid;
    logic  [8:0] scan_active_row;
    logic [24:0] scan_active_base_words;
    logic        scan_fill_active;
    logic        scan_fill_armed;
    logic        scan_fill_bank;
    logic  [8:0] scan_fill_row;
    logic [24:0] scan_fill_base_words;
    logic  [8:0] scan_fill_store_idx;
    logic        scan_fill_pop_d;
    logic [15:0] scan_rgb565_now;
    logic [24:0] sdram_rd_addr_cfg;
    logic [24:0] sdram_rd_max_addr_cfg;
    logic        sdram_rd_load_pulse;

    logic [16:0] copy_fb_next_addr;
    logic [16:0] copy_pixels_issued;
    logic [16:0] copy_words_written;
    logic        copy_fetch_inflight;
    logic  [7:0] copy_drain_count;
    logic        copy_word_pending_valid;
    logic [15:0] copy_word_pending;
    logic [24:0] sdram_wr_addr_cfg;
    logic [24:0] sdram_wr_max_addr_cfg;
    logic        sdram_wr_load_pulse;

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
    wire fifo_push_req = wr && (address >= ADDR_FIFO_LO) && (address < ADDR_FIFO_HI) && !fifo_full;
    wire desc_has_uv = desc_flags[FLAG_TEX_BIT];
    wire [5:0] fetch_target_words = ((fetch_count >= 6'd16) && desc_has_uv) ? 6'd32 : 6'd16;
    wire fifo_pop_req = (state == ST_FETCH) && (fetch_count < fetch_target_words) && !fifo_empty;
    wire engine_busy = (state != ST_IDLE);
    wire vsync_pulse = vga_vs_d & ~VGA_VS;
    wire [24:0] extmem_front_base_words = extmem_front_base[25:1];
    wire [24:0] extmem_back_base_words  = extmem_back_base[25:1];
    wire [24:0] display_base_words      = display_sel ? extmem_back_base_words : extmem_front_base_words;
    wire [24:0] copy_target_base_words  = copy_target_sel ? extmem_back_base_words : extmem_front_base_words;
    wire [8:0]  scan_current_row        = {1'b0, vcount[8:1]};
    wire [8:0]  scan_current_x          = hcount[10:2];
    wire        scan_current_x_valid    = (scan_current_x < FB_WIDTH);
    wire        scan_active_row_matches = scan_active_valid && (scan_active_row == scan_current_row);
    wire        scan_active_bank_ready  = scan_active_bank ? scan_line1_ready : scan_line0_ready;
    wire [8:0]  scan_active_bank_row    = scan_active_bank ? scan_line1_row : scan_line0_row;
    wire        scan_bank0_matches      = scan_line0_ready && (scan_line0_row == scan_current_row);
    wire        scan_bank1_matches      = scan_line1_ready && (scan_line1_row == scan_current_row);
    wire        scan_bank0_next_ready   = scan_line0_ready && (scan_line0_row == (scan_active_row + 9'd1));
    wire        scan_bank1_next_ready   = scan_line1_ready && (scan_line1_row == (scan_active_row + 9'd1));
    wire        scan_visible_data_ready = display_valid && sdram_ready &&
                                          scan_active_row_matches &&
                                          scan_active_bank_ready &&
                                          (scan_active_bank_row == scan_current_row);
    wire [8:0]  scan_fill_words_complete =
        scan_fill_store_idx + (scan_fill_pop_d ? 9'd1 : 9'd0);
    wire        scan_fill_line_done =
        scan_fill_pop_d && (scan_fill_words_complete == LINE_WORDS_9);
    wire        scan_fill_chunk_done =
        scan_fill_pop_d && (scan_fill_words_complete[5:0] == 6'd0);
    wire [24:0] scan_fill_next_chunk_base_words =
        scan_fill_base_words + {16'd0, scan_fill_words_complete};
    wire        sdram_wr_push = (state == ST_COPY) && copy_word_pending_valid && !sdram_wr_full;
    wire        sdram_rd_pop = scan_fill_active && !sdram_rd_empty &&
                               (scan_fill_words_complete < LINE_WORDS_9) &&
                               !scan_fill_chunk_done;
    // Leave scanout regular SDRAM read slots between copy write bursts.
    wire        copy_can_issue_read =
        (state == ST_COPY) &&
        (copy_pixels_issued < FB_PIXELS) &&
        !copy_fetch_inflight &&
        (sdram_wr_use[8:0] < COPY_WR_FIFO_HIGH_WATER) &&
        (!copy_word_pending_valid || sdram_wr_push);
    wire        copy_issue_read = copy_can_issue_read;
    wire [8:0]  sdram_wr_length_cfg = (state == ST_COPY) ? COPY_BURST_WORDS_9 : 9'd0;
    /*
     * Keep SDRAM reads in 64-word chunks. A 320-word line burst can cross the
     * SDRAM row/column boundary; 64-word chunks stay aligned because both the
     * frame line width and SDRAM column size are multiples of 64.
     */
    wire [8:0]  sdram_rd_length_cfg = scan_fill_armed ? READ_BURST_WORDS_9 : 9'd0;

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

    wire signed [10:0] draw_x_s = $signed({1'b0, draw_x_cur});
    wire signed  [9:0] draw_y_s = $signed({1'b0, draw_y_cur});
    /*
     * Keep the whole edge-function expression in signed arithmetic.
     * A manual sign-extension concat here becomes an unsigned operand,
     * which can flip the entire add tree into unsigned math and break
     * negative edge coefficients.
     */
    wire signed [63:0] edge_ax0 = $signed(edge_A[0]) * draw_x_s;
    wire signed [63:0] edge_by0 = $signed(edge_B[0]) * draw_y_s;
    wire signed [63:0] edge_c0  = $signed({{32{edge_C[0][31]}}, edge_C[0]});
    wire signed [63:0] edge_ax1 = $signed(edge_A[1]) * draw_x_s;
    wire signed [63:0] edge_by1 = $signed(edge_B[1]) * draw_y_s;
    wire signed [63:0] edge_c1  = $signed({{32{edge_C[1][31]}}, edge_C[1]});
    wire signed [63:0] edge_ax2 = $signed(edge_A[2]) * draw_x_s;
    wire signed [63:0] edge_by2 = $signed(edge_B[2]) * draw_y_s;
    wire signed [63:0] edge_c2  = $signed({{32{edge_C[2][31]}}, edge_C[2]});
    wire signed [63:0] edge_ax3 = $signed(edge_A[3]) * draw_x_s;
    wire signed [63:0] edge_by3 = $signed(edge_B[3]) * draw_y_s;
    wire signed [63:0] edge_c3  = $signed({{32{edge_C[3][31]}}, edge_C[3]});
    wire signed [63:0] edge_eval0 = edge_ax0 + edge_by0 + edge_c0;
    wire signed [63:0] edge_eval1 = edge_ax1 + edge_by1 + edge_c1;
    wire signed [63:0] edge_eval2 = edge_ax2 + edge_by2 + edge_c2;
    wire signed [63:0] edge_eval3 = edge_ax3 + edge_by3 + edge_c3;
    wire draw_inside = (edge_eval0 >= 0) && (edge_eval1 >= 0) &&
                       (edge_eval2 >= 0) && (edge_eval3 >= 0);
    wire signed [10:0] draw_dx_s = $signed({1'b0, draw_x_cur}) -
                                   $signed({1'b0, draw_x_min});
    wire signed  [9:0] draw_dy_s = $signed({1'b0, draw_y_cur}) -
                                   $signed({1'b0, draw_y_min});
    wire signed [47:0] draw_z_eval = $signed({32'd0, draw_z0}) +
                                     ($signed(draw_dz_dx) * draw_dx_s) +
                                     ($signed(draw_dz_dy) * draw_dy_s);
    wire [15:0] draw_z_value = clamp_z(draw_z_eval);
    wire signed [63:0] draw_uw_base = $signed({{32{draw_uw_0[31]}}, draw_uw_0});
    wire signed [63:0] draw_vw_base = $signed({{32{draw_vw_0[31]}}, draw_vw_0});
    wire signed [63:0] draw_iw_base = $signed({{32{draw_iw_0[31]}}, draw_iw_0});
    wire signed [63:0] draw_uw_eval = draw_uw_base +
                                      ($signed(draw_uw_dx) * draw_dx_s) +
                                      ($signed(draw_uw_dy) * draw_dy_s);
    wire signed [63:0] draw_vw_eval = draw_vw_base +
                                      ($signed(draw_vw_dx) * draw_dx_s) +
                                      ($signed(draw_vw_dy) * draw_dy_s);
    wire signed [63:0] draw_iw_eval = draw_iw_base +
                                      ($signed(draw_iw_dx) * draw_dx_s) +
                                      ($signed(draw_iw_dy) * draw_dy_s);
    wire signed [31:0] draw_uw_q = clamp_s32(draw_uw_eval);
    wire signed [31:0] draw_vw_q = clamp_s32(draw_vw_eval);
    wire [31:0] draw_iw_q = clamp_pos_u32(draw_iw_eval);
    wire [5:0] pipe0_iw_msb = msb_index32(pipe0_iw_q);
    wire [31:0] pipe0_iw_norm_q = (pipe0_iw_q == 32'd0) ? 32'd0 :
                                  (pipe0_iw_msb >= 6'd16) ?
                                  (pipe0_iw_q >> (pipe0_iw_msb - 6'd16)) :
                                  (pipe0_iw_q << (6'd16 - pipe0_iw_msb));
    wire [15:0] recip0_iw_phase = recip0_iw_norm_q[15:0];
    wire [10:0] recip0_iw_lut_idx = {1'b0, recip0_iw_phase[15:6]};
    wire [5:0] recip0_iw_lut_frac = recip0_iw_phase[5:0];
    wire [31:0] recip1_w_norm_delta = recip1_w_norm_lo - recip1_w_norm_hi;
    wire [37:0] recip1_w_interp_prod = recip1_w_norm_delta * recip1_iw_lut_frac;
    wire [37:0] recip1_w_interp_step_ext = (recip1_w_interp_prod + 38'd32) >> 6;
    wire [31:0] recip1_w_interp_step = recip1_w_interp_step_ext[31:0];
    wire [31:0] recip1_w_norm_q = recip1_w_norm_lo - recip1_w_interp_step;
    wire [31:0] recip2_w_q = recip2_iw_zero ? 32'd0 :
                             (recip2_iw_msb >= 6'd16) ?
                             (recip2_w_norm_q >> (recip2_iw_msb - 6'd16)) :
                             (recip2_w_norm_q << (6'd16 - recip2_iw_msb));
    wire signed [63:0] pipe1_u_prod = $signed(pipe1_uw_q) * $signed(pipe1_w_q);
    wire signed [63:0] pipe1_v_prod = $signed(pipe1_vw_q) * $signed(pipe1_w_q);
    wire tex0_repeat_uv = tex0_tex_or_color[6];
    wire [3:0] tex0_tex_u = texture_coord(tex0_u_prod, tex0_repeat_uv);
    wire [3:0] tex0_tex_v = texture_coord(tex0_v_prod, tex0_repeat_uv);
    wire [13:0] tex0_tex_addr = tex0_textured ?
                                 {tex0_tex_or_color[5:0], tex0_tex_v, tex0_tex_u} :
                                 14'd0;
    wire  [7:0] draw_pipe_raw_color = draw_pipe_textured ? tex_rd_data : draw_pipe_tex_or_color;
    wire  [7:0] draw_pipe_color = apply_light_bank(draw_pipe_raw_color, draw_pipe_light_bank);
    wire  [7:0] palette_src_addr = (state == ST_CLEAR) ? 8'd0 : draw_pipe_color;
    /* Palette reads for the draw pipeline are sampled into plr_* one
     * cycle before fog0 (see the plr_* register block). The
     * rgb888_to_rgb565 conversion is cheap bit slicing, so we leave it
     * as a combinational derivation on the registered palette output
     * and let fog0_src_rgb565 / fog0_fog_rgb565 pick it up on the next
     * cycle. */
    wire [15:0] plr_src_rgb565 = rgb888_to_rgb565(plr_src_rgb);
    wire [15:0] plr_fog_rgb565 = rgb888_to_rgb565(plr_fog_rgb);

    /* Separate combinational read used only during ST_CLEAR, which
     * writes the background color straight to the back buffer and does
     * not go through the draw pipeline. Address is always palette[0]
     * (background) while clearing. */
    wire [15:0] clear_rgb565 = rgb888_to_rgb565(palette[8'd0]);
    wire draw_pipe_transparent = draw_pipe_textured &&
                                 draw_pipe_alpha_key &&
                                 (draw_pipe_raw_color == 8'd0);
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
        $signed({1'b0, draw_pipe_x}) - 12'sd160;
    wire signed [10:0] draw_pipe_dy_center =
        11'sd120 - $signed({1'b0, draw_pipe_y});
    wire [23:0] draw_pipe_dx_sq = draw_pipe_dx_center * draw_pipe_dx_center;
    wire [23:0] draw_pipe_dy_sq = draw_pipe_dy_center * draw_pipe_dy_center;
    wire [24:0] draw_pipe_radius_sq = draw_pipe_dx_sq + draw_pipe_dy_sq;
    wire [40:0] draw_pipe_r2_prod = draw_pipe_radius_sq * fog_inv_proj_sq;
    wire [31:0] draw_pipe_r2_q16 = draw_pipe_r2_prod[31:0];
    wire [33:0] draw_pipe_ray_scale_q16 =
        34'd65536 + (({2'b00, draw_pipe_r2_q16} * 3'd3) >> 3);
    wire [65:0] fog0_radial_prod = fog0_w_q * fog0_ray_scale_q16;
    wire [31:0] fog0_radial_q16 = fog0_radial_prod[47:16];
    wire [15:0] fog0_radial_q8_8 = fog0_radial_q16[23:8];

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
    // Map active fog depth into the 4-level blend_rgb565 alpha scale
    // (0=no fog, 1=25%, 2=50%, 3=75%). Pixels past fog_end_dist bypass the
    // blend entirely and take the fog color directly.
    wire [1:0] fog1_fog_alpha =
        !fog1_fog_active ? 2'd0 :
        fog1_fog_full    ? 2'd0 :
        (fog1_radial_q8_8 < fog_dq1) ? 2'd1 :
        (fog1_radial_q8_8 < fog_dq2) ? 2'd2 : 2'd3;
    wire [15:0] fog1_fog_blended =
        blend_rgb565(fog1_src_rgb565, fog1_fog_rgb565, fog1_fog_alpha);
    wire [15:0] fog1_fogged_rgb565 =
        fog1_fog_full ? fog1_fog_rgb565 : fog1_fog_blended;
    wire [15:0] fog1_out_rgb565 =
        blend_rgb565(fog1_fogged_rgb565, fog1_dst_rgb565, fog1_alpha);
    wire draw_commit_pass = draw_pipe_inside &&
                            !draw_pipe_transparent &&
                            (!draw_pipe_ztest || (draw_pipe_z < draw_pipe_z_ref));
    wire [15:0] copy_rgb565 = fb_back_rd_data;

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
            else if (value > 16'sd319)
                clamp_x = 10'd319;
            else
                clamp_x = value[9:0];
        end
    endfunction

    function automatic [8:0] clamp_y(input logic signed [15:0] value);
        begin
            if (value < 0)
                clamp_y = 9'd0;
            else if (value > 16'sd239)
                clamp_y = 9'd239;
            else
                clamp_y = value[8:0];
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
        .WR_DATA     (copy_word_pending),
        .WR          (sdram_wr_push),
        .WR_ADDR     (sdram_wr_addr_cfg),
        .WR_MAX_ADDR (sdram_wr_max_addr_cfg),
        .WR_LENGTH   (sdram_wr_length_cfg),
        .WR_LOAD     (sdram_wr_load_pulse),
        .WR_CLK      (clk),
        .WR_FULL     (sdram_wr_full),
        .WR_USE      (sdram_wr_use),
        .RD_DATA     (sdram_rd_data),
        .RD          (sdram_rd_pop),
        .RD_ADDR     (sdram_rd_addr_cfg),
        .RD_MAX_ADDR (sdram_rd_max_addr_cfg),
        .RD_LENGTH   (sdram_rd_length_cfg),
        .RD_LOAD     (sdram_rd_load_pulse),
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

    voxel_sdp_ram #(
        .DATA_W(16),
        .ADDR_W(17),
        .DEPTH(FB_PIXELS)
    ) fb_back_ram (
        .clk     (clk),
        .rd_addr (fb_back_rd_addr),
        .rd_data (fb_back_rd_data),
        .wr_addr (fb_wr_addr),
        .wr_data (fb_wr_data),
        .wr_en   (fb_back_wr_en)
    );

    voxel_sdp_ram #(
        .DATA_W(16),
        .ADDR_W(17),
        .DEPTH(FB_PIXELS)
    ) z_ram (
        .clk     (clk),
        .rd_addr (z_rd_addr),
        .rd_data (z_rd_data),
        .wr_addr (z_wr_addr),
        .wr_data (z_wr_data),
        .wr_en   (z_wr_en)
    );

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
        .clk     (clk),
        .rd_addr (pipe2_tex_addr),
        .rd_data (tex_rd_data)
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
        clear_addr       = 17'd0;
        draw_row_base    = 17'd0;
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
        pipe0_valid      = 1'b0;
        pipe0_inside     = 1'b0;
        pipe0_ztest      = 1'b0;
        pipe0_textured   = 1'b0;
        pipe0_alpha_key  = 1'b0;
        pipe0_alpha      = 2'd0;
        pipe0_fog        = 1'b0;
        pipe0_light_bank = 2'd0;
        pipe0_tex_or_color = 8'd0;
        pipe0_addr       = 17'd0;
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
        recip0_addr      = 17'd0;
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
        recip1_addr      = 17'd0;
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
        recip2_addr      = 17'd0;
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
        pipe1_addr       = 17'd0;
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
        tex0_addr        = 17'd0;
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
        pipe2_addr       = 17'd0;
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
        draw_pipe_addr   = 17'd0;
        draw_pipe_z      = 16'd0;
        draw_pipe_z_ref  = 16'd0;
        draw_pipe_dst_rgb565 = 16'h0000;
        draw_pipe_x      = 10'd0;
        draw_pipe_y      = 9'd0;
        draw_pipe_w_q    = 32'd0;
        pal_rd_valid     = 1'b0;
        pal_rd_pass      = 1'b0;
        pal_rd_ztest     = 1'b0;
        pal_rd_alpha     = 2'd0;
        pal_rd_fog       = 1'b0;
        pal_rd_addr      = 17'd0;
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
        plr_addr         = 17'd0;
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
        fog0_addr        = 17'd0;
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
        fog1_addr        = 17'd0;
        fog1_z           = 16'd0;
        fog1_src_rgb565  = 16'h0000;
        fog1_dst_rgb565  = 16'h0000;
        fog1_fog_rgb565  = 16'h0000;
        fog1_radial_q8_8 = 16'd0;
        commit_valid     = 1'b0;
        commit_pass      = 1'b0;
        commit_ztest     = 1'b0;
        commit_addr      = 17'd0;
        commit_z         = 16'd0;
        commit_color     = 16'h0000;
        scan_rgb565_r    = 16'h0000;
        scan_visible_r   = 1'b0;
        copy_fb_rd_addr  = 17'd0;
        scan_line0_ready = 1'b0;
        scan_line1_ready = 1'b0;
        scan_line0_row   = 9'd0;
        scan_line1_row   = 9'd0;
        scan_line0_base_words = 25'd0;
        scan_line1_base_words = 25'd0;
        scan_active_bank = 1'b0;
        scan_active_valid = 1'b0;
        scan_active_row  = 9'd0;
        scan_active_base_words = 25'd0;
        scan_fill_active = 1'b0;
        scan_fill_armed  = 1'b0;
        scan_fill_bank   = 1'b0;
        scan_fill_row    = 9'd0;
        scan_fill_base_words = 25'd0;
        scan_fill_store_idx = 9'd0;
        scan_fill_pop_d  = 1'b0;
        sdram_rd_addr_cfg = 25'd0;
        sdram_rd_max_addr_cfg = 25'd0;
        sdram_rd_load_pulse = 1'b0;
        copy_fb_next_addr = 17'd0;
        copy_pixels_issued = 17'd0;
        copy_words_written = 17'd0;
        copy_fetch_inflight = 1'b0;
        copy_drain_count = 8'd0;
        copy_word_pending_valid = 1'b0;
        copy_word_pending = 16'd0;
        sdram_wr_addr_cfg = 25'd0;
        sdram_wr_max_addr_cfg = 25'd0;
        sdram_wr_load_pulse = 1'b0;
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
        end

        for (i = 0; i < LINE_WORDS; i = i + 1) begin
            scan_linebuf0[i] = 16'h0000;
            scan_linebuf1[i] = 16'h0000;
        end
    end

    always_comb begin
        if (!scan_current_x_valid)
            scan_rgb565_now = 16'h0000;
        else if (scan_active_bank)
            scan_rgb565_now = scan_linebuf1[scan_current_x];
        else
            scan_rgb565_now = scan_linebuf0[scan_current_x];

        scan_visible_now = VGA_BLANK_n && scan_visible_data_ready;
        if (!scan_visible_now)
            scan_rgb565_now = 16'h0000;

        fb_back_rd_addr = (state == ST_COPY) ?
                          (copy_issue_read ? copy_fb_next_addr : copy_fb_rd_addr) :
                          pipe0_addr;
        draw_addr = draw_row_base + {7'd0, draw_x_cur};
        fb_wr_addr = draw_addr;
        fb_wr_data = 16'h0000;
        fb_back_wr_en = 1'b0;
        z_rd_addr = pipe0_addr;
        z_wr_addr = draw_pipe_addr;
        z_wr_data = draw_pipe_z;
        z_wr_en   = 1'b0;

        case (state)
            ST_CLEAR: begin
                fb_wr_addr = clear_addr;
                fb_wr_data = clear_rgb565;
                z_wr_addr = clear_addr;
                z_wr_data = 16'hFFFF;
                z_wr_en   = 1'b1;
                fb_back_wr_en = 1'b1;
            end

            ST_DRAW,
            ST_DRAW_FLUSH: begin
                if (commit_valid && commit_pass) begin
                    fb_wr_addr = commit_addr;
                    fb_wr_data = commit_color;
                    fb_back_wr_en = 1'b1;
                end

                if (commit_valid && commit_pass && commit_ztest) begin
                    z_wr_en = 1'b1;
                    z_wr_addr = commit_addr;
                    z_wr_data = commit_z;
                end
            end

            default: ;
        endcase
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
            clear_addr       <= 17'd0;
            draw_row_base    <= 17'd0;
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
            pipe0_valid      <= 1'b0;
            pipe0_inside     <= 1'b0;
            pipe0_ztest      <= 1'b0;
            pipe0_textured   <= 1'b0;
            pipe0_alpha_key  <= 1'b0;
            pipe0_alpha      <= 2'd0;
            pipe0_fog        <= 1'b0;
            pipe0_light_bank <= 2'd0;
            pipe0_tex_or_color <= 8'd0;
            pipe0_addr       <= 17'd0;
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
            recip0_addr      <= 17'd0;
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
            recip1_addr      <= 17'd0;
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
            recip2_addr      <= 17'd0;
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
            pipe1_addr       <= 17'd0;
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
            tex0_addr        <= 17'd0;
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
            pipe2_addr       <= 17'd0;
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
            draw_pipe_addr   <= 17'd0;
            draw_pipe_z      <= 16'd0;
            draw_pipe_z_ref  <= 16'd0;
            draw_pipe_dst_rgb565 <= 16'h0000;
            draw_pipe_x      <= 10'd0;
            draw_pipe_y      <= 9'd0;
            draw_pipe_w_q    <= 32'd0;
            pal_rd_valid     <= 1'b0;
            pal_rd_pass      <= 1'b0;
            pal_rd_ztest     <= 1'b0;
            pal_rd_alpha     <= 2'd0;
            pal_rd_fog       <= 1'b0;
            pal_rd_addr      <= 17'd0;
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
            plr_addr         <= 17'd0;
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
            fog0_addr        <= 17'd0;
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
            fog1_addr        <= 17'd0;
            fog1_z           <= 16'd0;
            fog1_src_rgb565  <= 16'h0000;
            fog1_dst_rgb565  <= 16'h0000;
            fog1_fog_rgb565  <= 16'h0000;
            fog1_radial_q8_8 <= 16'd0;
            commit_valid     <= 1'b0;
            commit_pass      <= 1'b0;
            commit_ztest     <= 1'b0;
            commit_addr      <= 17'd0;
            commit_z         <= 16'd0;
            commit_color     <= 16'h0000;
            scan_rgb565_r    <= 16'h0000;
            copy_fb_rd_addr  <= 17'd0;
            scan_line0_ready <= 1'b0;
            scan_line1_ready <= 1'b0;
            scan_line0_row   <= 9'd0;
            scan_line1_row   <= 9'd0;
            scan_line0_base_words <= 25'd0;
            scan_line1_base_words <= 25'd0;
            scan_active_bank <= 1'b0;
            scan_active_valid <= 1'b0;
            scan_active_row  <= 9'd0;
            scan_active_base_words <= 25'd0;
            scan_fill_active <= 1'b0;
            scan_fill_armed  <= 1'b0;
            scan_fill_bank   <= 1'b0;
            scan_fill_row    <= 9'd0;
            scan_fill_base_words <= 25'd0;
            scan_fill_store_idx <= 9'd0;
            scan_fill_pop_d  <= 1'b0;
            sdram_rd_addr_cfg <= 25'd0;
            sdram_rd_max_addr_cfg <= 25'd0;
            sdram_rd_load_pulse <= 1'b0;
            copy_fb_next_addr <= 17'd0;
            copy_pixels_issued <= 17'd0;
            copy_words_written <= 17'd0;
            copy_fetch_inflight <= 1'b0;
            copy_drain_count <= 8'd0;
            copy_word_pending_valid <= 1'b0;
            copy_word_pending <= 16'd0;
            sdram_wr_addr_cfg <= 25'd0;
            sdram_wr_max_addr_cfg <= 25'd0;
            sdram_wr_load_pulse <= 1'b0;
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

            if (!sdram_ctrl_reset_n) begin
                if (sdram_powerup_counter == SDRAM_POWERUP_HOLD_CYCLES - 1) begin
                    sdram_ctrl_reset_n <= 1'b1;
                end else begin
                    sdram_powerup_counter <= sdram_powerup_counter + 18'd1;
                end
            end else if (!sdram_ready) begin
                if (sdram_init_wait_counter == SDRAM_INIT_WAIT_CYCLES - 1) begin
                    sdram_ready <= 1'b1;
                end else begin
                    sdram_init_wait_counter <= sdram_init_wait_counter + 16'd1;
                end
            end

            if (vsync_pulse) begin
                frame_count <= frame_count + 32'd1;
                scan_line0_ready <= 1'b0;
                scan_line1_ready <= 1'b0;
                scan_line0_base_words <= 25'd0;
                scan_line1_base_words <= 25'd0;
                scan_active_bank <= 1'b0;
                scan_active_valid <= 1'b0;
                scan_active_row <= 9'd0;
                scan_active_base_words <= 25'd0;
                scan_fill_active <= 1'b0;
                scan_fill_armed <= 1'b0;
                scan_fill_bank <= 1'b0;
                scan_fill_row <= 9'd0;
                scan_fill_base_words <= 25'd0;
                scan_fill_store_idx <= 9'd0;
                scan_fill_pop_d <= 1'b0;

                if (copy_complete_pending) begin
                    display_sel <= copy_target_sel;
                    display_valid <= 1'b1;
                    ctrl_flp_pending <= 1'b0;
                    copy_complete_pending <= 1'b0;
                    vsy_latch <= 1'b1;
                    if (sdram_ready) begin
                        scan_fill_active <= 1'b1;
                        scan_fill_armed <= 1'b1;
                        scan_fill_bank <= 1'b0;
                        scan_fill_row <= 9'd0;
                        scan_fill_base_words <= copy_target_base_words;
                        scan_fill_store_idx <= 9'd0;
                        sdram_rd_addr_cfg <= copy_target_base_words;
                        sdram_rd_max_addr_cfg <= copy_target_base_words + READ_BURST_WORDS_25;
                        sdram_rd_load_pulse <= 1'b1;
                    end
                end else begin
                    vsy_latch <= !ctrl_flp_pending;
                    if (display_valid && sdram_ready) begin
                        scan_fill_active <= 1'b1;
                        scan_fill_armed <= 1'b1;
                        scan_fill_bank <= 1'b0;
                        scan_fill_row <= 9'd0;
                        scan_fill_base_words <= display_base_words;
                        scan_fill_store_idx <= 9'd0;
                        sdram_rd_addr_cfg <= display_base_words;
                        sdram_rd_max_addr_cfg <= display_base_words + READ_BURST_WORDS_25;
                        sdram_rd_load_pulse <= 1'b1;
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

            if (!vsync_pulse) begin
                if (scan_fill_armed && !sdram_rd_load_pulse && !sdram_rd_empty)
                    scan_fill_armed <= 1'b0;

                if (scan_fill_pop_d) begin
                    if (scan_fill_bank)
                        scan_linebuf1[scan_fill_store_idx] <= sdram_rd_data;
                    else
                        scan_linebuf0[scan_fill_store_idx] <= sdram_rd_data;

                    if (scan_fill_line_done) begin
                        scan_fill_active <= 1'b0;
                        scan_fill_armed <= 1'b0;
                        if (scan_fill_bank) begin
                            scan_line1_ready <= 1'b1;
                            scan_line1_row <= scan_fill_row;
                            scan_line1_base_words <= scan_fill_base_words;
                        end else begin
                            scan_line0_ready <= 1'b1;
                            scan_line0_row <= scan_fill_row;
                            scan_line0_base_words <= scan_fill_base_words;
                        end

                        if (!scan_active_valid) begin
                            scan_active_valid <= 1'b1;
                            scan_active_bank <= scan_fill_bank;
                            scan_active_row <= scan_fill_row;
                            scan_active_base_words <= scan_fill_base_words;
                        end
                    end else begin
                        scan_fill_store_idx <= scan_fill_store_idx + 9'd1;
                        if (scan_fill_chunk_done) begin
                            scan_fill_armed <= 1'b1;
                            sdram_rd_addr_cfg <= scan_fill_next_chunk_base_words;
                            sdram_rd_max_addr_cfg <= scan_fill_next_chunk_base_words +
                                                     READ_BURST_WORDS_25;
                            sdram_rd_load_pulse <= 1'b1;
                        end
                    end
                end

                scan_fill_pop_d <= scan_fill_active ? sdram_rd_pop : 1'b0;

                if (display_valid && scan_active_valid && VGA_BLANK_n &&
                    (scan_current_row != scan_active_row)) begin
                    if (!scan_active_bank && scan_bank1_matches) begin
                        scan_active_bank <= 1'b1;
                        scan_active_row <= scan_current_row;
                        scan_active_base_words <= scan_line1_base_words;
                    end else if (scan_active_bank && scan_bank0_matches) begin
                        scan_active_bank <= 1'b0;
                        scan_active_row <= scan_current_row;
                        scan_active_base_words <= scan_line0_base_words;
                    end
                end

                if (!scan_fill_active && display_valid && sdram_ready && scan_active_valid &&
                    (scan_active_row < FB_HEIGHT - 1)) begin
                    if (!scan_active_bank && !scan_bank1_next_ready) begin
                        scan_fill_active <= 1'b1;
                        scan_fill_armed <= 1'b1;
                        scan_fill_bank <= 1'b1;
                        scan_fill_row <= scan_active_row + 9'd1;
                        scan_fill_base_words <= scan_active_base_words + LINE_WORDS_25;
                        scan_fill_store_idx <= 9'd0;
                        scan_line1_ready <= 1'b0;
                        sdram_rd_addr_cfg <= scan_active_base_words + LINE_WORDS_25;
                        sdram_rd_max_addr_cfg <= scan_active_base_words +
                                                 (LINE_WORDS_25 + READ_BURST_WORDS_25);
                        sdram_rd_load_pulse <= 1'b1;
                    end else if (scan_active_bank && !scan_bank0_next_ready) begin
                        scan_fill_active <= 1'b1;
                        scan_fill_armed <= 1'b1;
                        scan_fill_bank <= 1'b0;
                        scan_fill_row <= scan_active_row + 9'd1;
                        scan_fill_base_words <= scan_active_base_words + LINE_WORDS_25;
                        scan_fill_store_idx <= 9'd0;
                        scan_line0_ready <= 1'b0;
                        sdram_rd_addr_cfg <= scan_active_base_words + LINE_WORDS_25;
                        sdram_rd_max_addr_cfg <= scan_active_base_words +
                                                 (LINE_WORDS_25 + READ_BURST_WORDS_25);
                        sdram_rd_load_pulse <= 1'b1;
                    end
                end
            end

            if (sdram_wr_push) begin
                copy_word_pending_valid <= 1'b0;
                copy_words_written <= copy_words_written + 17'd1;
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

            commit_valid <= fog1_valid;
            commit_pass <= fog1_pass;
            commit_ztest <= fog1_ztest;
            commit_addr <= fog1_addr;
            commit_z <= fog1_z;
            commit_color <= fog1_out_rgb565;

            if (state == ST_COPY) begin
                if (copy_fetch_inflight &&
                    (!copy_word_pending_valid || sdram_wr_push)) begin
                    copy_word_pending <= copy_rgb565;
                    copy_word_pending_valid <= 1'b1;
                    copy_fetch_inflight <= 1'b0;
                end

                if (copy_issue_read) begin
                    copy_fb_rd_addr <= copy_fb_next_addr;
                    copy_fb_next_addr <= copy_fb_next_addr + 17'd1;
                    copy_pixels_issued <= copy_pixels_issued + 17'd1;
                    copy_fetch_inflight <= 1'b1;
                end
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
                    if (ctrl_flp_pending && sdram_ready && !copy_complete_pending) begin
                        state <= ST_COPY;
                        copy_target_sel <= ~display_sel;
                        copy_fb_next_addr <= 17'd0;
                        copy_pixels_issued <= 17'd0;
                        copy_words_written <= 17'd0;
                        copy_fetch_inflight <= 1'b0;
                        copy_drain_count <= 8'd0;
                        copy_word_pending_valid <= 1'b0;
                        copy_fb_rd_addr <= 17'd0;
                        sdram_wr_addr_cfg <= (~display_sel) ? extmem_back_base_words : extmem_front_base_words;
                        sdram_wr_max_addr_cfg <= ((~display_sel) ? extmem_back_base_words : extmem_front_base_words) + FB_WORDS_25;
                        sdram_wr_load_pulse <= 1'b1;
                    end else if (clear_pending) begin
                        state         <= ST_CLEAR;
                        clear_pending <= 1'b0;
                        clear_addr    <= 17'd0;
                    end else if (ctrl_en && (fifo_count >= 12'd16)) begin
                        state       <= ST_FETCH;
                        fetch_count <= 6'd0;
                    end
                end

                ST_CLEAR: begin
                    if (clear_addr == 17'd76799) begin
                        state <= ST_IDLE;
                    end else begin
                        clear_addr <= clear_addr + 17'd1;
                    end
                end

                ST_FETCH: begin
                    if (fetch_count == fetch_target_words) begin
                        draw_x_min <= desc_x_min;
                        draw_x_max <= desc_x_max;
                        draw_y_min <= desc_y_min;
                        draw_y_max <= desc_y_max;
                        draw_row_base <= {8'd0, desc_y_min} * 17'd320;
                        draw_x_cur <= desc_x_min;
                        draw_y_cur <= desc_y_min;
                        draw_tex_or_color <= desc_tex_or_color;
                        draw_flags <= desc_flags;
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
                        for (ei = 0; ei < 4; ei = ei + 1) begin
                            edge_A[ei] <= $signed(desc_words[2 + ei * 3]);
                            edge_B[ei] <= $signed(desc_words[3 + ei * 3]);
                            edge_C[ei] <= $signed(desc_words[4 + ei * 3]);
                        end

                        if ((desc_x_min > desc_x_max) || (desc_y_min > desc_y_max))
                            state <= ST_IDLE;
                        else
                            state <= ST_DRAW;
                    end else if (fifo_pop_req) begin
                        fetch_count <= fetch_count + 6'd1;
                    end
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
                    recip1_z_ref <= z_rd_data;
                    recip1_dst_rgb565 <= fb_back_rd_data;
                    recip1_x <= recip0_x;
                    recip1_y <= recip0_y;
                    recip1_uw_q <= recip0_uw_q;
                    recip1_vw_q <= recip0_vw_q;
                    recip1_iw_zero <= recip0_iw_zero;
                    recip1_iw_msb <= recip0_iw_msb;
                    recip1_iw_lut_frac <= recip0_iw_lut_frac;
                    recip1_w_norm_lo <= recip_lut[recip0_iw_lut_idx];
                    recip1_w_norm_hi <= recip_lut[recip0_iw_lut_idx + 11'd1];

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

                    if (state == ST_DRAW) begin
                        pipe0_valid <= 1'b1;
                        pipe0_inside <= draw_inside;
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

                        if (draw_x_cur == draw_x_max) begin
                            if (draw_y_cur == draw_y_max) begin
                                state <= ST_DRAW_FLUSH;
                                draw_flush_count <= DRAW_FLUSH_CYCLES;
                            end else begin
                                draw_row_base <= draw_row_base + 17'd320;
                                draw_x_cur <= draw_x_min;
                                draw_y_cur <= draw_y_cur + 9'd1;
                            end
                        end else begin
                            draw_x_cur <= draw_x_cur + 10'd1;
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
                        pipe0_addr <= 17'd0;
                        pipe0_z <= 16'd0;
                        pipe0_x <= 10'd0;
                        pipe0_y <= 9'd0;
                        pipe0_uw_q <= 32'sd0;
                        pipe0_vw_q <= 32'sd0;
                        pipe0_iw_q <= 32'd0;

                        if (draw_flush_count == 4'd1) begin
                            draw_flush_count <= 4'd0;
                            state <= ST_IDLE;
                        end else begin
                            draw_flush_count <= draw_flush_count - 4'd1;
                        end
                    end
                end

                ST_COPY: begin
                    if ((copy_words_written == FB_WORDS) &&
                        !copy_word_pending_valid &&
                        !copy_fetch_inflight &&
                        (sdram_wr_use[8:0] == 9'd0)) begin
                        if (copy_drain_count == COPY_DRAIN_CYCLES) begin
                            state <= ST_IDLE;
                            copy_complete_pending <= 1'b1;
                            copy_drain_count <= 8'd0;
                        end else begin
                            copy_drain_count <= copy_drain_count + 8'd1;
                        end
                    end else begin
                        copy_drain_count <= 8'd0;
                    end
                end

                default: state <= ST_IDLE;
            endcase

            extmem_dma_status <= {
                copy_words_written,
                3'h0,
                display_sel,
                copy_target_sel,
                copy_complete_pending,
                (state == ST_COPY),
                scan_fill_active,
                display_valid,
                sdram_ready,
                scan_active_bank,
                scan_fill_bank,
                scan_line1_ready,
                scan_line0_ready,
                1'b0
            };
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
