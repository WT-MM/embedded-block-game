// voxel_gpu.sv — bring-up MVP for the FPGA voxel GPU.
//
// This iteration implements the *register interface* and *VGA scanout*
// for real, with a visible test pattern, so the SW driver and
// /dev/voxel_gpu loop can be validated end-to-end on a monitor.
//
// What is implemented:
//   * Real Avalon-MM slave: CONTROL, STATUS, FRAME_COUNT, PALETTE_*.
//   * 256-entry × 24-bit palette RAM (writable from SW).
//   * 640×480 VGA timing driven from the 50 MHz Avalon clock.
//   * Status latches:
//       VSY  — set on next vsync after CONTROL.FLP, cleared on read of STATUS.
//       BSY  — pulses high for ~32 cycles after CONTROL.CLR.
//       FEM  — wired high (no real FIFO yet, so it is "always empty").
//       FFL  — wired low.
//       FIFO_COUNT — wired to 0.
//   * FRAME_COUNT increments on every vsync.
//   * Test pattern: 16 horizontal bars, each one row of palette RAM.
//     A 64-pixel "flip indicator" square drifts horizontally by 4 px
//     every successful FLIP, so the user can visually confirm vsync +
//     FRAME_COUNT advancement.
//
// What is intentionally NOT implemented:
//   * FIFO storage. Writes to the FIFO_WINDOW are silently dropped.
//   * Rasterizer / Z-buffer / framebuffer. The pattern is generated
//     directly in the scanout block.
//   * Interrupts.
//
// Register map (byte offsets / word addresses; address port is WORDS):
//   0x000  CONTROL      [3]=CLR [2]=IEN [1]=FLP [0]=EN          (R/W)
//   0x004  STATUS       [19:4]=FIFO_COUNT [3]=VSY [2]=FEM
//                       [1]=FFL [0]=BSY                          (R)
//   0x008  FRAME_COUNT  free-running                             (R)
//   0x00C  PALETTE_ADDR [7:0]                                    (W)
//   0x010  PALETTE_DATA [23:16]=R [15:8]=G [7:0]=B               (W)
//   0x1000..0x2FFF  FIFO_WINDOW                                  (W, ignored)

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

    // ----------------------------------------------------------------
    // Address decode (word addresses).
    //   0x000..0x004 = registers
    //   0x400..0xBFF = FIFO window (8 KB / 2048 words)
    // ----------------------------------------------------------------
    localparam logic [12:0] ADDR_CONTROL  = 13'h000;
    localparam logic [12:0] ADDR_STATUS   = 13'h001;
    localparam logic [12:0] ADDR_FRAMECNT = 13'h002;
    localparam logic [12:0] ADDR_PAL_ADDR = 13'h003;
    localparam logic [12:0] ADDR_PAL_DATA = 13'h004;

    // ----------------------------------------------------------------
    // Register state
    // ----------------------------------------------------------------
    logic        ctrl_en;
    logic        ctrl_flp;       // pulse, auto-clears after vsync
    logic        ctrl_ien;
    logic        ctrl_clr;       // pulse, auto-clears after BSY drains
    logic [31:0] frame_count;
    logic  [7:0] pal_addr;

    // 256-entry palette RAM. Quartus will infer block RAM.
    (* ramstyle = "M10K" *) logic [23:0] palette [0:255];

    // Status latches
    logic       vsy_latch;
    logic [5:0] bsy_counter;     // counts down while BSY is asserted
    wire        bsy = (bsy_counter != 6'd0);

    // ----------------------------------------------------------------
    // VGA timing
    // ----------------------------------------------------------------
    logic [10:0] hcount;
    logic  [9:0] vcount;

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

    // Detect the start of vertical blanking (falling edge of VGA_VS).
    // Synthesizable VGA_VS is active-low, so the falling edge marks the
    // beginning of the sync pulse — perfect "frame done" tick.
    logic vga_vs_d;
    always_ff @(posedge clk) vga_vs_d <= VGA_VS;
    wire vsync_pulse = vga_vs_d & ~VGA_VS;

    // ----------------------------------------------------------------
    // Avalon write side: registers + palette + control pulses.
    // ----------------------------------------------------------------
    wire wr = chipselect & write;

    // Default-init the palette to a vibrant rainbow so even before SW
    // uploads anything you see something on screen. Indices 0..15 are
    // hand-picked so the test bars look distinct; 16..255 fall back to a
    // grayscale ramp.
    integer i;
    initial begin
        ctrl_en     = 1'b0;
        ctrl_flp    = 1'b0;
        ctrl_ien    = 1'b0;
        ctrl_clr    = 1'b0;
        pal_addr    = 8'h0;
        frame_count = 32'h0;
        vsy_latch   = 1'b0;
        bsy_counter = 6'h0;

        palette[0]  = 24'h202028;   // dark slate (background)
        palette[1]  = 24'hFF0000;   // red
        palette[2]  = 24'hFF8000;   // orange
        palette[3]  = 24'hFFFF00;   // yellow
        palette[4]  = 24'h80FF00;   // chartreuse
        palette[5]  = 24'h00FF00;   // green
        palette[6]  = 24'h00FF80;   // mint
        palette[7]  = 24'h00FFFF;   // cyan
        palette[8]  = 24'h0080FF;   // sky blue
        palette[9]  = 24'h0000FF;   // blue
        palette[10] = 24'h8000FF;   // purple
        palette[11] = 24'hFF00FF;   // magenta
        palette[12] = 24'hFF0080;   // pink
        palette[13] = 24'hFFFFFF;   // white
        palette[14] = 24'h808080;   // medium gray
        palette[15] = 24'hFFFF80;   // pale yellow (flip indicator)
        for (i = 16; i < 256; i = i + 1) begin
            palette[i] = { i[7:0], i[7:0], i[7:0] };
        end
    end

    always_ff @(posedge clk) begin
        if (reset) begin
            ctrl_en     <= 1'b0;
            ctrl_flp    <= 1'b0;
            ctrl_ien    <= 1'b0;
            ctrl_clr    <= 1'b0;
            pal_addr    <= 8'h0;
            frame_count <= 32'h0;
            vsy_latch   <= 1'b0;
            bsy_counter <= 6'h0;
        end else begin
            // ----- BSY pulse drain (continuous) -----
            if (bsy_counter != 6'd0) begin
                bsy_counter <= bsy_counter - 6'd1;
                if (bsy_counter == 6'd1) begin
                    ctrl_clr <= 1'b0;       // auto-clear CLR
                end
            end

            // ----- Vsync handling -----
            // VSY is "any vsync has happened since you armed me by writing
            // CTRL.FLP=1". The Avalon write block below runs *after* this
            // and wins on collisions, so a write-on-vsync still arms cleanly
            // for the next vsync.
            if (vsync_pulse) begin
                frame_count <= frame_count + 32'd1;
                vsy_latch   <= 1'b1;
                ctrl_flp    <= 1'b0;        // auto-clear FLP latch
            end

            // ----- Avalon writes (placed last so they win NBA collisions) -----
            if (wr) begin
                case (address)
                    ADDR_CONTROL: begin
                        ctrl_en  <= writedata[0];
                        ctrl_ien <= writedata[2];
                        if (writedata[1]) begin
                            // FLP arm: latch the request and clear VSY so
                            // the *next* vsync re-sets it.
                            ctrl_flp  <= 1'b1;
                            vsy_latch <= 1'b0;
                        end
                        if (writedata[3]) begin
                            ctrl_clr    <= 1'b1;
                            bsy_counter <= 6'd32;   // ~32 clk BSY pulse
                            vsy_latch   <= 1'b0;
                        end
                    end
                    ADDR_PAL_ADDR: pal_addr           <= writedata[7:0];
                    ADDR_PAL_DATA: palette[pal_addr]  <= writedata[23:0];
                    default      : ;  // STATUS/FRAMECNT/FIFO writes ignored
                endcase
            end
        end
    end

    // ----------------------------------------------------------------
    // Avalon read side
    // ----------------------------------------------------------------
    wire [31:0] status_word = {
        12'h0,            // [31:20] reserved
        16'h0,            // [19:4]  FIFO_COUNT (0, no FIFO)
        vsy_latch,        // [3]     VSY
        1'b1,             // [2]     FEM (always empty: no FIFO)
        1'b0,             // [1]     FFL
        bsy               // [0]     BSY
    };

    wire [31:0] control_word = {
        28'h0, ctrl_clr, ctrl_ien, ctrl_flp, ctrl_en
    };

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

    // ----------------------------------------------------------------
    // Scanout: visible test pattern.
    //
    //   * Background = palette[0].
    //   * 16 horizontal bars (each 30 px tall): bar k = palette[k+1].
    //     This makes voxel_test's 5 SET_PALETTE calls light up rows
    //     1..5 instantly.
    //   * 32×32 "flip indicator" square that drifts 4 px to the right
    //     each successful FLIP, wrapping around. Color = palette[15].
    //     This proves FLIP + FRAME_COUNT are alive *visually*.
    // ----------------------------------------------------------------

    // hcount[10:1] is the pixel column 0..639 (25 MHz pixel clock from
    // the 50 MHz system clock). vcount is the row 0..479.
    wire [9:0] px_x = hcount[10:1];
    wire [9:0] px_y = vcount;

    // Bar index 0..15 (16 bars × 30 rows = 480 rows).
    wire [3:0] bar = px_y[8:5];   // px_y / 32; rows 480..511 don't render

    // Flip indicator position: scroll_x advances by 4 each FLIP.
    logic [9:0] scroll_x;
    always_ff @(posedge clk) begin
        if (reset) begin
            scroll_x <= 10'd0;
        end else if (vsync_pulse && ctrl_flp) begin
            // Same edge that latches VSY also bumps the indicator.
            scroll_x <= (scroll_x + 10'd4 >= 10'd640) ? 10'd0
                                                     : scroll_x + 10'd4;
        end
    end

    wire in_indicator = (px_x >= scroll_x) && (px_x < scroll_x + 10'd32) &&
                        (px_y >= 10'd360) && (px_y < 10'd392);

    logic [7:0] pal_index;
    always_comb begin
        if (in_indicator) begin
            pal_index = 8'd15;
        end else begin
            // 16 bars indexed 1..16 to skip palette[0] (background).
            pal_index = {4'h0, bar} + 8'h1;
        end
    end

    wire [23:0] pixel_rgb = palette[pal_index];

    always_comb begin
        if (VGA_BLANK_n) begin
            VGA_R = pixel_rgb[23:16];
            VGA_G = pixel_rgb[15: 8];
            VGA_B = pixel_rgb[ 7: 0];
        end else begin
            VGA_R = 8'h00;
            VGA_G = 8'h00;
            VGA_B = 8'h00;
        end
    end

endmodule

// ====================================================================
// 640x480 VGA timing from a 50 MHz clock. Cribbed from the lab3
// vga_ball reference; renamed to avoid module-name collision if both
// IPs are ever instantiated in the same system.
// ====================================================================
module voxel_vga_counters (
    input  logic        clk50,
    input  logic        reset,
    output logic [10:0] hcount,        // hcount[10:1] is the pixel column
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
    always_ff @(posedge clk50 or posedge reset)
        if (reset)          hcount <= 11'd0;
        else if (endOfLine) hcount <= 11'd0;
        else                hcount <= hcount + 11'd1;
    assign endOfLine = (hcount == HTOTAL - 11'd1);

    logic endOfField;
    always_ff @(posedge clk50 or posedge reset)
        if (reset)          vcount <= 10'd0;
        else if (endOfLine)
            if (endOfField) vcount <= 10'd0;
            else            vcount <= vcount + 10'd1;
    assign endOfField = (vcount == VTOTAL - 10'd1);

    assign VGA_HS = !((hcount[10:8] == 3'b101) & !(hcount[7:5] == 3'b111));
    assign VGA_VS = !(vcount[9:1] == (VACTIVE + VFRONT_PORCH) / 2);

    assign VGA_SYNC_n  = 1'b0;
    assign VGA_BLANK_n = !(hcount[10] & (hcount[9] | hcount[8])) &
                        !(vcount[9] | (vcount[8:5] == 4'b1111));
    assign VGA_CLK     = hcount[0];   // 25 MHz pixel clock

endmodule
