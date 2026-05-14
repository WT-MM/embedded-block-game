// Even/odd-bank wrapper around voxel_sdp_ram.

module voxel_banked_sdp_ram #(
    parameter int DATA_W = 16,
    parameter int ADDR_W = 16,
    parameter int DEPTH  = 40960
) (
    input  logic                clk,
    input  logic [ADDR_W-1:0]   rd_addr_e,
    output logic [DATA_W-1:0]   rd_data_e,
    input  logic [ADDR_W-1:0]   rd_addr_o,
    output logic [DATA_W-1:0]   rd_data_o,
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
