// voxel_gpu.sv — top-level peripheral for hardware-accelerated block game
//
// Register map (Avalon MM slave, byte-addressed, writedata 32 bits):
//   Offset 0x00: CONTROL (R/W)  [3]=CLR [2]=IEN [1]=FLP [0]=EN
//   Offset 0x04: STATUS  (R)    [19:4]=FIFO_COUNT [3]=VSY [2]=FEM [1]=FFL [0]=BSY
//   Offset 0x08: FRAME_COUNT (R)   32-bit free-running frame counter
//   Offset 0x0C: PALETTE_ADDR (W)  8 bits in [7:0]
//   Offset 0x10: PALETTE_DATA (W)  [23:16]=R [15:8]=G [7:0]=B
//   Offsets 0x1000..0x2FFF: FIFO_WINDOW (W) — command FIFO, 16 words per quad
//
// Address port carries byte addresses (because Platform Designer address units
// = WORDS, and we declare writedata as 32 bits, so address[0] = byte offset 0,
// address[1] = byte offset 4, etc.). Address port is 13 bits to cover 0x2FFF.

module voxel_gpu(
    input  logic        clk,
    input  logic        reset,

    // Avalon MM slave
    input  logic [12:0] address,       // byte offset / 4 (word addresses)
    input  logic        chipselect,
    input  logic        write,
    input  logic [31:0] writedata,
    input  logic  [3:0] byteenable,
    output logic [31:0] readdata,

    // VGA conduit (to board pins)
    output logic [7:0]  VGA_R,
    output logic [7:0]  VGA_G,
    output logic [7:0]  VGA_B,
    output logic        VGA_CLK,
    output logic        VGA_HS,
    output logic        VGA_VS,
    output logic        VGA_BLANK_n,
    output logic        VGA_SYNC_n
);

    // TODO: instantiate submodules (rasterizer, framebuffer, palette, vga_scanout, ...)
    // For bring-up, just produce a solid color and respond to register reads so
    // Platform Designer / Quartus / kernel driver plumbing can be tested.

    // Stub: drive readdata to 0, VGA outputs to safe defaults.
    assign readdata    = 32'h0;
    assign VGA_R       = 8'h00;
    assign VGA_G       = 8'h00;
    assign VGA_B       = 8'h00;
    assign VGA_CLK     = 1'b0;
    assign VGA_HS      = 1'b0;
    assign VGA_VS      = 1'b0;
    assign VGA_BLANK_n = 1'b0;
    assign VGA_SYNC_n  = 1'b0;

endmodule
