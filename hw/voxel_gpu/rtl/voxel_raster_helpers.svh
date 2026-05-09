    function automatic [9:0] clamp_x(input logic signed [15:0] value);
        begin
            if (value < 0)
                clamp_x = 10'd0;
            else if (value > 16'sd639)
                clamp_x = 10'd639;
            else
                clamp_x = value[9:0];
        end
    endfunction

    function automatic [8:0] clamp_y(input logic signed [15:0] value);
        begin
            if (value < 0)
                clamp_y = 9'd0;
            else if (value > 16'sd479)
                clamp_y = 9'd479;
            else
                clamp_y = value[8:0];
        end
    endfunction

    function automatic [15:0] band_local_addr(input logic [9:0] x,
                                              input logic [8:0] y,
                                              input logic [2:0] band);
        logic [8:0] band_base_y;
        logic [8:0] local_y;
        begin
            band_base_y = band_base_row(band);
            local_y = (y >= band_base_y) ? (y - band_base_y) : 9'd0;
            band_local_addr = local_y * 16'd640 + {6'd0, x};
        end
    endfunction

    function automatic [15:0] band_pixel_count(input logic [2:0] band);
        logic [9:0]  base_row;
        logic [9:0]  band_end;
        logic [15:0] visible_rows;
        begin
            base_row = {1'b0, band_base_row(band)};
            band_end = base_row + 10'd60;
            if (base_row >= 10'd480)
                band_pixel_count = 16'd0;
            else if (band_end > 10'd480) begin
                visible_rows = {6'd0, (10'd480 - base_row)};
                band_pixel_count = visible_rows * 16'd640;
            end else begin
                band_pixel_count = 16'd38400;
            end
        end
    endfunction

    function automatic [15:0] band_local_row_offset(input logic [5:0] local_y);
        begin
            band_local_row_offset = {local_y, 9'd0} + {3'd0, local_y, 7'd0};
        end
    endfunction

    function automatic [15:0] band_row_window_count(input logic [5:0] y_min,
                                                    input logic [5:0] y_max);
        logic [6:0] rows;
        begin
            rows = {1'b0, y_max} - {1'b0, y_min} + 7'd1;
            band_row_window_count = {rows, 9'd0} + {2'd0, rows, 7'd0};
        end
    endfunction

    function automatic [24:0] band_word_count(input logic [2:0] band);
        begin
            band_word_count = {9'd0, band_pixel_count(band)};
        end
    endfunction

    function automatic [24:0] band_word_offset(input logic [2:0] band);
        begin
            band_word_offset = {7'd0, band, 15'd0} +
                               {10'd0, band, 12'd0} +
                               {12'd0, band, 10'd0} +
                               {13'd0, band, 9'd0};
        end
    endfunction

    function automatic [8:0] band_base_row(input logic [2:0] band);
        begin
            band_base_row = {band, 6'd0} - {4'd0, band, 2'd0};
        end
    endfunction

    function automatic [4:0] sky_clear_start_index(input logic [2:0] band);
        begin
            case (band)
                3'd0: sky_clear_start_index = 5'd0;
                3'd1: sky_clear_start_index = 5'd3;
                3'd2: sky_clear_start_index = 5'd6;
                3'd3: sky_clear_start_index = 5'd9;
                3'd4: sky_clear_start_index = 5'd12;
                3'd5: sky_clear_start_index = 5'd15;
                3'd6: sky_clear_start_index = 5'd18;
                default: sky_clear_start_index = 5'd21;
            endcase
        end
    endfunction

    function automatic [4:0] sky_row_count_for_local_y(input logic [5:0] local_y);
        logic [4:0] local_mod;
        begin
            local_mod = local_y[4:0];
            if (local_y >= 6'd40)
                sky_row_count_for_local_y = local_mod - 5'd8;
            else if (local_y >= 6'd32)
                sky_row_count_for_local_y = local_mod + 5'd12;
            else if (local_y >= 6'd20)
                sky_row_count_for_local_y = local_mod - 5'd20;
            else
                sky_row_count_for_local_y = local_mod;
        end
    endfunction

    function automatic [4:0] sky_clear_index_for_local_y(input logic [2:0] band,
                                                         input logic [5:0] local_y);
        logic [5:0] pal;
        begin
            pal = {1'b0, sky_clear_start_index(band)};
            if (local_y >= 6'd40)
                pal = pal + 6'd2;
            else if (local_y >= 6'd20)
                pal = pal + 6'd1;
            sky_clear_index_for_local_y =
                (pal >= SKY_GRADIENT_COLORS_6) ?
                SKY_GRADIENT_LAST_INDEX : pal[4:0];
        end
    endfunction

    function automatic [4:0] sky_palette_for_y(input logic [8:0] y);
        logic [2:0] band;
        logic [8:0] local_y_wide;
        logic [5:0] local_y;
        logic [6:0] row_sum;
        logic [1:0] pal_offset;
        logic [5:0] pal;
        begin
            band = y_to_band(y);
            local_y_wide = y - band_base_row(band);
            local_y = local_y_wide[5:0];
            row_sum = {1'b0, local_y};
            if (row_sum >= 7'd60)
                pal_offset = 2'd3;
            else if (row_sum >= 7'd40)
                pal_offset = 2'd2;
            else if (row_sum >= 7'd20)
                pal_offset = 2'd1;
            else
                pal_offset = 2'd0;

            pal = {1'b0, sky_clear_start_index(band)} +
                  {4'b0, pal_offset};
            sky_palette_for_y = (pal >= SKY_GRADIENT_COLORS_6) ?
                                SKY_GRADIENT_LAST_INDEX : pal[4:0];
        end
    endfunction

    function automatic [2:0] y_to_band(input logic [8:0] y);
        logic [9:0] y_adjusted;
        begin
            y_adjusted = {1'b0, y} + {5'd0, y[8:5], 1'b0} + 10'd2;
            y_to_band = y_adjusted[8:6];
        end
    endfunction

    function automatic [15:0] clamp_z(input logic signed [47:0] value);
        begin
            if (value < 0)
                clamp_z = 16'h0000;
            else if (value > 48'sd65534)
                clamp_z = Z_VALID_FAR;
            else
                clamp_z = value[15:0];
        end
    endfunction

    function automatic signed [31:0] clamp_s32(input logic signed [63:0] value);
        begin
            if (value > 64'sh7FFF_FFFF)
                clamp_s32 = 32'sh7FFF_FFFF;
            else if (value < -64'sh8000_0000)
                clamp_s32 = -32'sh8000_0000;
            else
                clamp_s32 = value[31:0];
        end
    endfunction

    function automatic [31:0] clamp_pos_u32(input logic signed [63:0] value);
        begin
            if (value <= 0)
                clamp_pos_u32 = 32'd0;
            else if (value > 64'sh7FFF_FFFF)
                clamp_pos_u32 = 32'h7FFF_FFFF;
            else
                clamp_pos_u32 = value[31:0];
        end
    endfunction

    function automatic [3:0] texture_coord(input logic signed [63:0] value,
                                           input logic repeat_uv);
        begin
            if (repeat_uv)
                texture_coord = value[35:32];
            else if (value <= 64'sd0)
                texture_coord = 4'd0;
            else if (value >= 64'sh0000_0010_0000_0000)
                texture_coord = 4'd15;
            else
                texture_coord = value[35:32];
        end
    endfunction
