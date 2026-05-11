// Pure combinational raster setup math used by voxel_gpu.sv.

module voxel_raster_setup (
    input  logic        [9:0]  draw_x_start_even,
    input  logic        [8:0]  draw_y_min,
    input  logic        [9:0]  draw_x_min,
    input  logic        [15:0] draw_z0,
    input  logic signed [15:0] draw_dz_dx,
    input  logic signed [31:0] draw_uw_0,
    input  logic signed [31:0] draw_uw_dx,
    input  logic signed [31:0] draw_vw_0,
    input  logic signed [31:0] draw_vw_dx,
    input  logic signed [31:0] draw_iw_0,
    input  logic signed [31:0] draw_iw_dx,
    input  logic signed [31:0] edge_a0,
    input  logic signed [31:0] edge_a1,
    input  logic signed [31:0] edge_a2,
    input  logic signed [31:0] edge_a3,
    input  logic signed [31:0] edge_b0,
    input  logic signed [31:0] edge_b1,
    input  logic signed [31:0] edge_b2,
    input  logic signed [31:0] edge_b3,
    input  logic signed [31:0] edge_c0,
    input  logic signed [31:0] edge_c1,
    input  logic signed [31:0] edge_c2,
    input  logic signed [31:0] edge_c3,
    output logic signed [63:0] edge_eval0,
    output logic signed [63:0] edge_eval1,
    output logic signed [63:0] edge_eval2,
    output logic signed [63:0] edge_eval3,
    output logic signed [47:0] z_start_val,
    output logic signed [63:0] uw_start_val,
    output logic signed [63:0] vw_start_val,
    output logic signed [63:0] iw_start_val
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

    wire signed [10:0] setup_start_x = $signed({1'b0, draw_x_start_even});
    wire signed  [9:0] setup_start_y = $signed({1'b0, draw_y_min});

    wire signed [63:0] edge_ax0 = $signed(edge_a0) * setup_start_x;
    wire signed [63:0] edge_ax1 = $signed(edge_a1) * setup_start_x;
    wire signed [63:0] edge_ax2 = $signed(edge_a2) * setup_start_x;
    wire signed [63:0] edge_ax3 = $signed(edge_a3) * setup_start_x;
    wire signed [63:0] edge_by0 = $signed(edge_b0) * setup_start_y;
    wire signed [63:0] edge_by1 = $signed(edge_b1) * setup_start_y;
    wire signed [63:0] edge_by2 = $signed(edge_b2) * setup_start_y;
    wire signed [63:0] edge_by3 = $signed(edge_b3) * setup_start_y;

    wire signed [47:0] draw_dz_dx_ext = sext16_to_s48(draw_dz_dx);
    wire signed [63:0] draw_uw_dx_ext = sext32_to_s64(draw_uw_dx);
    wire signed [63:0] draw_vw_dx_ext = sext32_to_s64(draw_vw_dx);
    wire signed [63:0] draw_iw_dx_ext = sext32_to_s64(draw_iw_dx);

    always_comb begin
        edge_eval0 = edge_ax0 + edge_by0 + sext32_to_s64(edge_c0);
        edge_eval1 = edge_ax1 + edge_by1 + sext32_to_s64(edge_c1);
        edge_eval2 = edge_ax2 + edge_by2 + sext32_to_s64(edge_c2);
        edge_eval3 = edge_ax3 + edge_by3 + sext32_to_s64(edge_c3);

        z_start_val =
            $signed({32'd0, draw_z0}) -
            (draw_x_min[0] ? draw_dz_dx_ext : 48'sd0);
        uw_start_val =
            sext32_to_s64(draw_uw_0) -
            (draw_x_min[0] ? draw_uw_dx_ext : 64'sd0);
        vw_start_val =
            sext32_to_s64(draw_vw_0) -
            (draw_x_min[0] ? draw_vw_dx_ext : 64'sd0);
        iw_start_val =
            sext32_to_s64(draw_iw_0) -
            (draw_x_min[0] ? draw_iw_dx_ext : 64'sd0);
    end
endmodule
