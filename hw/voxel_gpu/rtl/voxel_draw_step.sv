// Pure combinational per-pixel/pair raster stepping math used by voxel_gpu.sv.

module voxel_draw_step (
    input  logic signed [63:0] edge_cur0,
    input  logic signed [63:0] edge_cur1,
    input  logic signed [63:0] edge_cur2,
    input  logic signed [63:0] edge_cur3,
    input  logic signed [63:0] edge_row0,
    input  logic signed [63:0] edge_row1,
    input  logic signed [63:0] edge_row2,
    input  logic signed [63:0] edge_row3,
    input  logic signed [31:0] edge_a0,
    input  logic signed [31:0] edge_a1,
    input  logic signed [31:0] edge_a2,
    input  logic signed [31:0] edge_a3,
    input  logic signed [31:0] edge_b0,
    input  logic signed [31:0] edge_b1,
    input  logic signed [31:0] edge_b2,
    input  logic signed [31:0] edge_b3,
    input  logic signed [47:0] z_cur,
    input  logic signed [47:0] z_row,
    input  logic signed [15:0] dz_dx,
    input  logic signed [15:0] dz_dy,
    input  logic signed [63:0] uw_cur,
    input  logic signed [63:0] uw_row,
    input  logic signed [31:0] uw_dx,
    input  logic signed [31:0] uw_dy,
    input  logic signed [63:0] vw_cur,
    input  logic signed [63:0] vw_row,
    input  logic signed [31:0] vw_dx,
    input  logic signed [31:0] vw_dy,
    input  logic signed [63:0] iw_cur,
    input  logic signed [63:0] iw_row,
    input  logic signed [31:0] iw_dx,
    input  logic signed [31:0] iw_dy,
    output logic signed [63:0] edge_lane1_0,
    output logic signed [63:0] edge_lane1_1,
    output logic signed [63:0] edge_lane1_2,
    output logic signed [63:0] edge_lane1_3,
    output logic signed [63:0] edge_next_pair0,
    output logic signed [63:0] edge_next_pair1,
    output logic signed [63:0] edge_next_pair2,
    output logic signed [63:0] edge_next_pair3,
    output logic signed [63:0] edge_next_row0,
    output logic signed [63:0] edge_next_row1,
    output logic signed [63:0] edge_next_row2,
    output logic signed [63:0] edge_next_row3,
    output logic signed [47:0] z_lane1,
    output logic signed [47:0] z_next_pair,
    output logic signed [47:0] z_next_row,
    output logic signed [63:0] uw_lane1,
    output logic signed [63:0] uw_next_pair,
    output logic signed [63:0] uw_next_row,
    output logic signed [63:0] vw_lane1,
    output logic signed [63:0] vw_next_pair,
    output logic signed [63:0] vw_next_row,
    output logic signed [63:0] iw_lane1,
    output logic signed [63:0] iw_next_pair,
    output logic signed [63:0] iw_next_row
);
    function automatic signed [63:0] sext32_to_s64(input logic signed [31:0] value);
        begin
            sext32_to_s64 = {{32{value[31]}}, value};
        end
    endfunction

    function automatic signed [47:0] sext16_to_s48(input logic signed [15:0] value);
        begin
            sext16_to_s48 = {{32{value[15]}}, value};
        end
    endfunction

    wire signed [63:0] edge_a0_ext = sext32_to_s64(edge_a0);
    wire signed [63:0] edge_a1_ext = sext32_to_s64(edge_a1);
    wire signed [63:0] edge_a2_ext = sext32_to_s64(edge_a2);
    wire signed [63:0] edge_a3_ext = sext32_to_s64(edge_a3);
    wire signed [63:0] edge_b0_ext = sext32_to_s64(edge_b0);
    wire signed [63:0] edge_b1_ext = sext32_to_s64(edge_b1);
    wire signed [63:0] edge_b2_ext = sext32_to_s64(edge_b2);
    wire signed [63:0] edge_b3_ext = sext32_to_s64(edge_b3);
    wire signed [47:0] dz_dx_ext = sext16_to_s48(dz_dx);
    wire signed [47:0] dz_dy_ext = sext16_to_s48(dz_dy);
    wire signed [63:0] uw_dx_ext = sext32_to_s64(uw_dx);
    wire signed [63:0] uw_dy_ext = sext32_to_s64(uw_dy);
    wire signed [63:0] vw_dx_ext = sext32_to_s64(vw_dx);
    wire signed [63:0] vw_dy_ext = sext32_to_s64(vw_dy);
    wire signed [63:0] iw_dx_ext = sext32_to_s64(iw_dx);
    wire signed [63:0] iw_dy_ext = sext32_to_s64(iw_dy);

    always_comb begin
        edge_lane1_0 = edge_cur0 + edge_a0_ext;
        edge_lane1_1 = edge_cur1 + edge_a1_ext;
        edge_lane1_2 = edge_cur2 + edge_a2_ext;
        edge_lane1_3 = edge_cur3 + edge_a3_ext;

        edge_next_pair0 = edge_cur0 + edge_a0_ext + edge_a0_ext;
        edge_next_pair1 = edge_cur1 + edge_a1_ext + edge_a1_ext;
        edge_next_pair2 = edge_cur2 + edge_a2_ext + edge_a2_ext;
        edge_next_pair3 = edge_cur3 + edge_a3_ext + edge_a3_ext;

        edge_next_row0 = edge_row0 + edge_b0_ext;
        edge_next_row1 = edge_row1 + edge_b1_ext;
        edge_next_row2 = edge_row2 + edge_b2_ext;
        edge_next_row3 = edge_row3 + edge_b3_ext;

        z_lane1 = z_cur + dz_dx_ext;
        z_next_pair = z_cur + dz_dx_ext + dz_dx_ext;
        z_next_row = z_row + dz_dy_ext;

        uw_lane1 = uw_cur + uw_dx_ext;
        uw_next_pair = uw_cur + uw_dx_ext + uw_dx_ext;
        uw_next_row = uw_row + uw_dy_ext;

        vw_lane1 = vw_cur + vw_dx_ext;
        vw_next_pair = vw_cur + vw_dx_ext + vw_dx_ext;
        vw_next_row = vw_row + vw_dy_ext;

        iw_lane1 = iw_cur + iw_dx_ext;
        iw_next_pair = iw_cur + iw_dx_ext + iw_dx_ext;
        iw_next_row = iw_row + iw_dy_ext;
    end
endmodule
