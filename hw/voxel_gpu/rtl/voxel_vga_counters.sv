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

    parameter HIMAGE_SHIFT_CYCLES = 11'd16; // 8 VGA pixels, moves image right.
    parameter HACTIVE      = 11'd1280;
    parameter HFRONT_PORCH = 11'd32 - HIMAGE_SHIFT_CYCLES;
    parameter HSYNC        = 11'd192;
    parameter HBACK_PORCH  = 11'd96 + HIMAGE_SHIFT_CYCLES;
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

    wire vga_hs_out = ~((hcount >= (HACTIVE + HFRONT_PORCH)) &&
                        (hcount <  (HACTIVE + HFRONT_PORCH + HSYNC)));
    wire vga_vs_out = ~((vcount >= (VACTIVE + VFRONT_PORCH)) &&
                        (vcount <  (VACTIVE + VFRONT_PORCH + VSYNC)));
    wire vga_blank_out = (hcount < HACTIVE) && (vcount < VACTIVE);
    wire vga_sync_out  = 1'b0;

    always_ff @(posedge clk50) begin
        if (reset) begin
            VGA_HS <= 1'b1;
            VGA_VS <= 1'b1;
            VGA_BLANK_n <= 1'b0;
            VGA_SYNC_n <= 1'b0;
        end else begin
            VGA_HS <= vga_hs_out;
            VGA_VS <= vga_vs_out;
            VGA_BLANK_n <= vga_blank_out;
            VGA_SYNC_n <= vga_sync_out;
        end
    end

    // Invert vs hcount[0]: DAC rising edge mid-pixel eye for ADV7123 setup/hold.
    // This clock goes directly to the DAC, so it must not be delayed.
    assign VGA_CLK     = ~hcount[0];

endmodule
