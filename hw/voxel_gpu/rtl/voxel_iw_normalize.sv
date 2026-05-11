// Normalize a positive Q value so bit 16 is the leading one.

module voxel_iw_normalize (
    input  logic [31:0] iw_q,
    output logic [5:0]  iw_msb,
    output logic [31:0] iw_norm_q
);
    function automatic [5:0] msb_index32(input logic [31:0] value);
        integer bit_idx;
        logic found;
        begin
            msb_index32 = 6'd0;
            found = 1'b0;
            for (bit_idx = 31; bit_idx >= 0; bit_idx = bit_idx - 1) begin
                if (!found && value[bit_idx]) begin
                    msb_index32 = bit_idx[5:0];
                    found = 1'b1;
                end
            end
        end
    endfunction

    wire [5:0] iw_msb_calc = msb_index32(iw_q);

    always_comb begin
        iw_msb = iw_msb_calc;
        iw_norm_q =
            (iw_q == 32'd0) ? 32'd0 :
            (iw_msb_calc >= 6'd16) ?
            (iw_q >> (iw_msb_calc - 6'd16)) :
            (iw_q << (6'd16 - iw_msb_calc));
    end
endmodule
