// Restore an interpolated normalized reciprocal value to the original scale.

module voxel_w_denormalize (
    input  logic        iw_zero,
    input  logic [5:0]  iw_msb,
    input  logic [31:0] w_norm_q,
    output logic [31:0] w_q
);
    always_comb begin
        w_q =
            iw_zero ? 32'd0 :
            (iw_msb >= 6'd16) ?
            (w_norm_q >> (iw_msb - 6'd16)) :
            (w_norm_q << (6'd16 - iw_msb));
    end
endmodule
