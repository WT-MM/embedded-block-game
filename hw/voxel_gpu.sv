// voxel_gpu.sv — first real end-to-end render path for the FPGA voxel GPU.
//
// This version is intentionally scoped to the first monitor-visible milestone:
//   * Real Avalon-MM register file + 2048-word command FIFO.
//   * 256-entry RGB palette programmable from software.
//   * Double-buffered 320x240 8-bit framebuffer, scanned out as 640x480
//     via 2x2 pixel doubling.
//   * CLEAR command clears the back buffer to palette index 0.
//   * FLIP swaps front/back on the next vsync.
//   * Descriptor fetch consumes 16 FIFO words (64 bytes) per quad.
//   * Rasterizer currently supports flat-color quads only:
//       - bbox walk
//       - 4 edge-function inclusion test
//       - writes tex_or_color as a palette index
//
// Intentionally not implemented yet:
//   * Z-buffer / depth test
//   * Texturing / UV block consumption
//   * Interrupts / waitrequest backpressure

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
    output logic        VGA_SYNC_n
);

    localparam logic [12:0] ADDR_CONTROL  = 13'h000;
    localparam logic [12:0] ADDR_STATUS   = 13'h001;
    localparam logic [12:0] ADDR_FRAMECNT = 13'h002;
    localparam logic [12:0] ADDR_PAL_ADDR = 13'h003;
    localparam logic [12:0] ADDR_PAL_DATA = 13'h004;
    localparam logic [12:0] ADDR_FIFO_LO  = 13'h400;  // 0x1000
    localparam logic [12:0] ADDR_FIFO_HI  = 13'hC00;  // 0x3000 (exclusive)

    localparam int FB_WIDTH     = 320;
    localparam int FB_HEIGHT    = 240;
    localparam int FB_PIXELS    = FB_WIDTH * FB_HEIGHT;
    localparam int FIFO_DEPTH   = 2048;
    localparam int QUAD_WORDS   = 16;

    typedef enum logic [1:0] {
        ST_IDLE  = 2'd0,
        ST_CLEAR = 2'd1,
        ST_FETCH = 2'd2,
        ST_DRAW  = 2'd3
    } engine_state_t;

    engine_state_t state;

    logic        ctrl_en;
    logic        ctrl_ien;
    logic        ctrl_flp_pending;
    logic        clear_pending;
    logic [31:0] frame_count;
    logic  [7:0] pal_addr;
    logic        vsy_latch;
    logic        front_sel;  // 0 => fb0 is front, 1 => fb1 is front

    (* ramstyle = "M10K" *) logic [23:0] palette [0:255];
    (* ramstyle = "M10K, no_rw_check" *) logic [7:0] fb0 [0:FB_PIXELS-1];
    (* ramstyle = "M10K, no_rw_check" *) logic [7:0] fb1 [0:FB_PIXELS-1];
    logic [31:0] fifo_mem [0:FIFO_DEPTH-1];

    logic [10:0] fifo_wr_ptr;
    logic [10:0] fifo_rd_ptr;
    logic [11:0] fifo_count;
    wire         fifo_full  = (fifo_count == 12'd2048);
    wire         fifo_empty = (fifo_count == 0);
    wire [31:0]  fifo_head  = fifo_mem[fifo_rd_ptr];

    logic [31:0] desc_words [0:QUAD_WORDS-1];
    logic  [4:0] fetch_count;

    logic [16:0] clear_addr;
    logic  [9:0] draw_x_min, draw_x_max, draw_x_cur;
    logic  [8:0] draw_y_max, draw_y_cur;
    logic  [7:0] draw_color;
    logic signed [31:0] edge_A [0:3];
    logic signed [31:0] edge_B [0:3];
    logic signed [31:0] edge_C [0:3];

    logic [10:0] hcount;
    logic  [9:0] vcount;

    logic [16:0] scan_addr_now;
    logic        scan_visible_now;
    logic  [7:0] scan_idx_r;
    logic        scan_visible_r;
    logic [16:0] scan_row_base;
    logic [16:0] draw_addr;

    logic vga_vs_d;

    wire wr = chipselect & write;
    wire fifo_push_req = wr && (address >= ADDR_FIFO_LO) && (address < ADDR_FIFO_HI) && !fifo_full;
    wire fifo_pop_req = (state == ST_FETCH) && (fetch_count < 5'd16) && !fifo_empty;
    wire engine_busy = (state != ST_IDLE);
    wire vsync_pulse = vga_vs_d & ~VGA_VS;
    wire _unused_byteenable = &{1'b0, byteenable};
    wire _unused_counter_bits = &{1'b0, hcount[1:0], vcount[9], vcount[0]};

    wire signed [15:0] desc_x_min_raw = $signed(desc_words[0][15:0]);
    wire signed [15:0] desc_y_min_raw = $signed(desc_words[0][31:16]);
    wire signed [15:0] desc_x_max_raw = $signed(desc_words[1][15:0]);
    wire signed [15:0] desc_y_max_raw = $signed(desc_words[1][31:16]);
    wire [9:0] desc_x_min = clamp_x(desc_x_min_raw);
    wire [9:0] desc_x_max = clamp_x(desc_x_max_raw);
    wire [8:0] desc_y_min = clamp_y(desc_y_min_raw);
    wire [8:0] desc_y_max = clamp_y(desc_y_max_raw);
    wire [7:0] desc_color = desc_words[15][23:16];

    wire signed [10:0] draw_x_s = $signed({1'b0, draw_x_cur});
    wire signed  [9:0] draw_y_s = $signed({1'b0, draw_y_cur});
    wire signed [63:0] edge_eval0 = edge_A[0] * draw_x_s + edge_B[0] * draw_y_s + {{32{edge_C[0][31]}}, edge_C[0]};
    wire signed [63:0] edge_eval1 = edge_A[1] * draw_x_s + edge_B[1] * draw_y_s + {{32{edge_C[1][31]}}, edge_C[1]};
    wire signed [63:0] edge_eval2 = edge_A[2] * draw_x_s + edge_B[2] * draw_y_s + {{32{edge_C[2][31]}}, edge_C[2]};
    wire signed [63:0] edge_eval3 = edge_A[3] * draw_x_s + edge_B[3] * draw_y_s + {{32{edge_C[3][31]}}, edge_C[3]};
    wire draw_inside = (edge_eval0 >= 0) && (edge_eval1 >= 0) &&
                       (edge_eval2 >= 0) && (edge_eval3 >= 0);

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

    wire [23:0] pixel_rgb = palette[scan_idx_r];

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

    integer i;
    integer ei;
    initial begin
        ctrl_en          = 1'b0;
        ctrl_ien         = 1'b0;
        ctrl_flp_pending = 1'b0;
        clear_pending    = 1'b0;
        frame_count      = 32'h0;
        pal_addr         = 8'h0;
        vsy_latch        = 1'b0;
        front_sel        = 1'b0;
        state            = ST_IDLE;
        fifo_wr_ptr      = 11'd0;
        fifo_rd_ptr      = 11'd0;
        fifo_count       = 12'd0;
        fetch_count      = 5'd0;
        clear_addr       = 17'd0;
        draw_x_min       = 10'd0;
        draw_x_max       = 10'd0;
        draw_x_cur       = 10'd0;
        draw_y_max       = 9'd0;
        draw_y_cur       = 9'd0;
        draw_color       = 8'd0;
        scan_idx_r       = 8'd0;
        scan_visible_r   = 1'b0;
        vga_vs_d         = 1'b1;

        palette[0]  = 24'h202028;
        palette[1]  = 24'hFF0000;
        palette[2]  = 24'h00FF00;
        palette[3]  = 24'h0080FF;
        palette[4]  = 24'hFFFF00;
        palette[5]  = 24'hFFFFFF;
        palette[6]  = 24'h00FFFF;
        palette[7]  = 24'hFF00FF;
        palette[8]  = 24'h808080;
        palette[9]  = 24'h804000;
        palette[10] = 24'h80FF00;
        palette[11] = 24'h0000FF;
        palette[12] = 24'hFF8000;
        palette[13] = 24'hFFFFFF;
        palette[14] = 24'h404040;
        palette[15] = 24'hFFFF80;
        for (i = 16; i < 256; i = i + 1)
            palette[i] = {i[7:0], i[7:0], i[7:0]};

        for (i = 0; i < FB_PIXELS; i = i + 1) begin
            fb0[i] = 8'd0;
            fb1[i] = 8'd0;
        end

        for (i = 0; i < FIFO_DEPTH; i = i + 1)
            fifo_mem[i] = 32'h0;

        for (i = 0; i < QUAD_WORDS; i = i + 1)
            desc_words[i] = 32'h0;

        for (i = 0; i < 4; i = i + 1) begin
            edge_A[i] = 32'sd0;
            edge_B[i] = 32'sd0;
            edge_C[i] = 32'sd0;
        end
    end

    always_comb begin
        if (VGA_BLANK_n) begin
            scan_visible_now = 1'b1;
            scan_row_base = {9'd0, vcount[8:1]} * 17'd320;
            scan_addr_now = scan_row_base + {7'd0, hcount[10:2]};
        end else begin
            scan_visible_now = 1'b0;
            scan_row_base = 17'd0;
            scan_addr_now = 17'd0;
        end
        draw_addr = ({8'd0, draw_y_cur} * 17'd320) + {7'd0, draw_x_cur};
    end

    always_ff @(posedge clk) begin
        vga_vs_d <= VGA_VS;

        if (front_sel)
            scan_idx_r <= fb1[scan_addr_now];
        else
            scan_idx_r <= fb0[scan_addr_now];
        scan_visible_r <= scan_visible_now;
    end

    always_ff @(posedge clk) begin
        if (reset) begin
            ctrl_en          <= 1'b0;
            ctrl_ien         <= 1'b0;
            ctrl_flp_pending <= 1'b0;
            clear_pending    <= 1'b0;
            frame_count      <= 32'h0;
            pal_addr         <= 8'h0;
            vsy_latch        <= 1'b0;
            front_sel        <= 1'b0;
            state            <= ST_IDLE;
            fifo_wr_ptr      <= 11'd0;
            fifo_rd_ptr      <= 11'd0;
            fifo_count       <= 12'd0;
            fetch_count      <= 5'd0;
            clear_addr       <= 17'd0;
            draw_x_min       <= 10'd0;
            draw_x_max       <= 10'd0;
            draw_x_cur       <= 10'd0;
            draw_y_max       <= 9'd0;
            draw_y_cur       <= 9'd0;
            draw_color       <= 8'd0;
            for (ei = 0; ei < 4; ei = ei + 1) begin
                edge_A[ei] <= 32'sd0;
                edge_B[ei] <= 32'sd0;
                edge_C[ei] <= 32'sd0;
            end
        end else begin
            if (vsync_pulse) begin
                frame_count <= frame_count + 32'd1;
                vsy_latch   <= 1'b1;
                if (ctrl_flp_pending) begin
                    front_sel        <= ~front_sel;
                    ctrl_flp_pending <= 1'b0;
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
                    default: ;
                endcase
            end

            if (fifo_push_req)
                fifo_mem[fifo_wr_ptr] <= writedata;
            if (fifo_pop_req)
                desc_words[fetch_count[3:0]] <= fifo_head;

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

            case (state)
                ST_IDLE: begin
                    fetch_count <= 5'd0;
                    if (clear_pending) begin
                        state         <= ST_CLEAR;
                        clear_pending <= 1'b0;
                        clear_addr    <= 17'd0;
                    end else if (ctrl_en && (fifo_count >= 12'd16)) begin
                        state       <= ST_FETCH;
                        fetch_count <= 5'd0;
                    end
                end

                ST_CLEAR: begin
                    if (front_sel)
                        fb0[clear_addr] <= 8'd0;
                    else
                        fb1[clear_addr] <= 8'd0;

                    if (clear_addr == 17'd76799) begin
                        state <= ST_IDLE;
                    end else begin
                        clear_addr <= clear_addr + 17'd1;
                    end
                end

                ST_FETCH: begin
                    if (fetch_count == 5'd16) begin
                        draw_x_min <= desc_x_min;
                        draw_x_max <= desc_x_max;
                        draw_y_max <= desc_y_max;
                        draw_x_cur <= desc_x_min;
                        draw_y_cur <= desc_y_min;
                        draw_color <= desc_color;
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
                        fetch_count <= fetch_count + 5'd1;
                    end
                end

                ST_DRAW: begin
                    if (draw_inside) begin
                        if (front_sel)
                            fb0[draw_addr] <= draw_color;
                        else
                            fb1[draw_addr] <= draw_color;
                    end

                    if (draw_x_cur == draw_x_max) begin
                        if (draw_y_cur == draw_y_max) begin
                            state <= ST_IDLE;
                        end else begin
                            draw_x_cur <= draw_x_min;
                            draw_y_cur <= draw_y_cur + 9'd1;
                        end
                    end else begin
                        draw_x_cur <= draw_x_cur + 10'd1;
                    end
                end

                default: state <= ST_IDLE;
            endcase
        end
    end

    always_comb begin
        case (address)
            ADDR_CONTROL : readdata = control_word;
            ADDR_STATUS  : readdata = status_word;
            ADDR_FRAMECNT: readdata = frame_count;
            ADDR_PAL_ADDR: readdata = {24'h0, pal_addr};
            ADDR_PAL_DATA: readdata = {8'h0, palette[pal_addr]};
            default      : readdata = 32'h0;
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

// ====================================================================
// 640x480 VGA timing from a 50 MHz clock. hcount[10:1] corresponds to
// the 640 visible pixel columns at the 25 MHz VGA rate.
// ====================================================================
module voxel_vga_counters (
    input  logic        clk50,
    input  logic        reset,
    output logic [10:0] hcount,
    output logic  [9:0] vcount,
    output logic        VGA_CLK,
    output logic        VGA_HS,
    output logic        VGA_VS,
    output logic        VGA_BLANK_n,
    output logic        VGA_SYNC_n
);

    parameter HACTIVE      = 11'd1280;
    parameter HFRONT_PORCH = 11'd32;
    parameter HSYNC        = 11'd192;
    parameter HBACK_PORCH  = 11'd96;
    parameter HTOTAL       = HACTIVE + HFRONT_PORCH + HSYNC + HBACK_PORCH;

    parameter VACTIVE      = 10'd480;
    parameter VFRONT_PORCH = 10'd10;
    parameter VSYNC        = 10'd2;
    parameter VBACK_PORCH  = 10'd33;
    parameter VTOTAL       = VACTIVE + VFRONT_PORCH + VSYNC + VBACK_PORCH;

    logic endOfLine;
    logic endOfField;

    always_ff @(posedge clk50)
        if (reset)          hcount <= 11'd0;
        else if (endOfLine) hcount <= 11'd0;
        else                hcount <= hcount + 11'd1;

    assign endOfLine = (hcount == HTOTAL - 11'd1);

    always_ff @(posedge clk50)
        if (reset) begin
            vcount <= 10'd0;
        end else if (endOfLine) begin
            if (endOfField)
                vcount <= 10'd0;
            else
                vcount <= vcount + 10'd1;
        end

    assign endOfField = (vcount == VTOTAL - 10'd1);

    assign VGA_HS = !((hcount[10:8] == 3'b101) & !(hcount[7:5] == 3'b111));
    assign VGA_VS = !(vcount[9:1] == 9'd245);

    assign VGA_SYNC_n  = 1'b0;
    assign VGA_BLANK_n = !(hcount[10] & (hcount[9] | hcount[8])) &
                         !(vcount[9] | (vcount[8:5] == 4'b1111));
    assign VGA_CLK     = hcount[0];

endmodule
