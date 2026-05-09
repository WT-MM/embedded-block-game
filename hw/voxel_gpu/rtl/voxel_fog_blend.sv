// Pure combinational fog + translucency blend for one pixel lane.

module voxel_fog_blend (
    input  logic        fog_enable,
    input  logic        pixel_fog,
    input  logic [15:0] radial_q8_8,
    input  logic [15:0] fog_start_dist,
    input  logic [15:0] fog_end_dist,
    input  logic [15:0] src_rgb565,
    input  logic [15:0] dst_rgb565,
    input  logic [15:0] fog_rgb565,
    input  logic [1:0]  alpha,
    output logic [15:0] out_rgb565
);
`include "voxel_color_helpers.svh"

    wire fog_active =
        fog_enable && pixel_fog && (fog_end_dist > fog_start_dist) &&
        (radial_q8_8 > fog_start_dist);
    wire fog_full = fog_active && (radial_q8_8 >= fog_end_dist);
    wire [15:0] fog_range = fog_end_dist - fog_start_dist;
    wire [15:0] fog_q1 = fog_start_dist + {2'b00, fog_range[15:2]};
    wire [15:0] fog_q2 = fog_start_dist + {1'b0, fog_range[15:1]};
    wire [1:0] fog_alpha =
        !fog_active ? 2'd0 :
        fog_full    ? 2'd0 :
        (radial_q8_8 < fog_q1) ? 2'd1 :
        (radial_q8_8 < fog_q2) ? 2'd2 : 2'd3;
    wire [15:0] fog_blended = blend_rgb565(src_rgb565, fog_rgb565, fog_alpha);
    wire [15:0] fogged_rgb565 = fog_full ? fog_rgb565 : fog_blended;

    assign out_rgb565 = blend_rgb565(fogged_rgb565, dst_rgb565, alpha);
endmodule
