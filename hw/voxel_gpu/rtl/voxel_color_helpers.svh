    function automatic [15:0] rgb888_to_rgb565(input logic [23:0] rgb888);
        begin
            rgb888_to_rgb565 = {
                rgb888[23:19],
                rgb888[15:10],
                rgb888[7:3]
            };
        end
    endfunction

    function automatic [15:0] blend_rgb565(
        input logic [15:0] src,
        input logic [15:0] dst,
        input logic  [1:0] alpha
    );
        logic [6:0] r_mix;
        logic [7:0] g_mix;
        logic [6:0] b_mix;
        begin
            case (alpha)
                2'd1: begin  // 75% source, 25% destination
                    r_mix = ({2'd0, src[15:11]} * 3'd3) + {2'd0, dst[15:11]} + 7'd2;
                    g_mix = ({2'd0, src[10:5]} * 3'd3) + {2'd0, dst[10:5]} + 8'd2;
                    b_mix = ({2'd0, src[4:0]} * 3'd3) + {2'd0, dst[4:0]} + 7'd2;
                    blend_rgb565 = {r_mix[6:2], g_mix[7:2], b_mix[6:2]};
                end
                2'd2: begin  // 50% source, 50% destination
                    r_mix = {1'd0, src[15:11]} + {1'd0, dst[15:11]} + 7'd1;
                    g_mix = {1'd0, src[10:5]} + {1'd0, dst[10:5]} + 8'd1;
                    b_mix = {1'd0, src[4:0]} + {1'd0, dst[4:0]} + 7'd1;
                    blend_rgb565 = {r_mix[5:1], g_mix[6:1], b_mix[5:1]};
                end
                2'd3: begin  // 25% source, 75% destination
                    r_mix = {2'd0, src[15:11]} + ({2'd0, dst[15:11]} * 3'd3) + 7'd2;
                    g_mix = {2'd0, src[10:5]} + ({2'd0, dst[10:5]} * 3'd3) + 8'd2;
                    b_mix = {2'd0, src[4:0]} + ({2'd0, dst[4:0]} * 3'd3) + 7'd2;
                    blend_rgb565 = {r_mix[6:2], g_mix[7:2], b_mix[6:2]};
                end
                default: blend_rgb565 = src;
            endcase
        end
    endfunction

    function automatic [7:0] expand5_to_8(input logic [4:0] value);
        begin
            expand5_to_8 = {value, value[4:2]};
        end
    endfunction

    function automatic [7:0] expand6_to_8(input logic [5:0] value);
        begin
            expand6_to_8 = {value, value[5:4]};
        end
    endfunction

    function automatic [7:0] apply_light_bank(input logic [7:0] color,
                                              input logic [1:0] light_bank);
        begin
            if ((color == 8'd0) || (light_bank == 2'd0) || (color[7:6] != 2'b00))
                apply_light_bank = color;
            else
                apply_light_bank = {light_bank, color[5:0]};
        end
    endfunction
