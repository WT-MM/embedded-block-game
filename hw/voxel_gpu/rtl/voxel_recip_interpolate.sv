// Interpolate adjacent reciprocal LUT entries using the normalized IW fraction.

module voxel_recip_interpolate (
    input  logic [31:0] w_norm_lo,
    input  logic [31:0] w_norm_hi,
    input  logic [5:0]  iw_lut_frac,
    output logic [31:0] w_norm_q
);
    wire [31:0] w_norm_delta = w_norm_lo - w_norm_hi;
    wire [37:0] w_interp_prod = w_norm_delta * iw_lut_frac;
    wire [37:0] w_interp_step_ext = (w_interp_prod + 38'd32) >> 6;
    wire [31:0] w_interp_step = w_interp_step_ext[31:0];

    assign w_norm_q = w_norm_lo - w_interp_step;
endmodule
