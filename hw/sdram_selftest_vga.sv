module sdram_selftest_vga (
    input  logic        clk50,
    input  logic        reset_req,

    output logic [12:0] DRAM_ADDR,
    output logic  [1:0] DRAM_BA,
    output logic        DRAM_CAS_N,
    output logic        DRAM_CKE,
    output logic        DRAM_CLK,
    output logic        DRAM_CS_N,
    inout  wire [15:0]  DRAM_DQ,
    output logic        DRAM_LDQM,
    output logic        DRAM_RAS_N,
    output logic        DRAM_UDQM,
    output logic        DRAM_WE_N,

    output logic  [7:0] VGA_B,
    output logic        VGA_BLANK_N,
    output logic        VGA_CLK,
    output logic  [7:0] VGA_G,
    output logic        VGA_HS,
    output logic  [7:0] VGA_R,
    output logic        VGA_SYNC_N,
    output logic        VGA_VS,

    output logic        test_running,
    output logic        test_pass,
    output logic        test_fail,
    output logic  [3:0] test_state
);

    localparam int POWERUP_HOLD_CYCLES = 18'd200000;
    localparam int INIT_WAIT_CYCLES    = 16'd32000;
    localparam int TEST_WORDS          = 1024;
    localparam int TEST_LAST_ADDR      = TEST_WORDS - 1;
    localparam int BURST_WORDS         = 16;
    localparam int TITLE_X             = 200;
    localparam int TITLE_Y             = 88;
    localparam int STATUS_X            = 260;
    localparam int STATUS_Y            = 156;
    localparam int TITLE_SCALE         = 4;
    localparam int STATUS_SCALE        = 4;
    localparam int CELL_W              = 6;
    localparam int FONT_W              = 5;
    localparam int FONT_H              = 7;
    localparam int TITLE_CHARS         = 10;
    localparam int STATUS_CHARS        = 5;
    localparam int BAR_X               = 80;
    localparam int BAR_Y               = 284;
    localparam int BAR_W               = 480;
    localparam int BAR_H               = 24;
    localparam logic [24:0] TEST_WORDS_25     = 25'd1024;
    localparam logic [24:0] TEST_LAST_ADDR_25 = 25'd1023;
    localparam logic  [8:0] BURST_WORDS_9     = 9'd16;

    typedef enum logic [2:0] {
        STATUS_RESET = 3'd0,
        STATUS_WRITE = 3'd1,
        STATUS_READ  = 3'd2,
        STATUS_PASS  = 3'd3,
        STATUS_FAIL  = 3'd4
    } status_t;

    logic        ctrl_reset_n = 1'b0;
    logic        wr_req;
    logic [15:0] wr_data;
    logic        rd_req;
    logic [15:0] rd_data;
    logic [24:0] current_address;
    logic        rw_pass;
    logic        rw_fail;
    logic        rw_done;
    logic  [3:0] rw_state;
    logic        rw_same;
    logic        start_n = 1'b1;
    logic        global_reset_n = 1'b0;
    logic [17:0] powerup_counter = '0;
    logic [15:0] init_counter = '0;
    logic        ctrl_clk;
    logic        wr_full;
    logic        rd_empty;
    logic [15:0] wr_used;
    logic [15:0] rd_used;
    logic  [1:0] dram_cs_n_bus;

    logic [10:0] hcount;
    logic  [9:0] vcount;
    logic [9:0] pixel_x;
    logic [9:0] pixel_y;
    logic       pixel_visible;
    logic       title_on;
    logic       status_on;
    logic       bar_border_on;
    logic       bar_fill_on;
    logic [23:0] bg_rgb;
    logic [23:0] pixel_rgb;
    logic [31:0] progress_words;
    logic [9:0]  bar_fill_px;
    logic [4:0]  glyph_row;
    status_t     status_code;

    always_ff @(posedge clk50 or posedge reset_req) begin
        if (reset_req) begin
            powerup_counter <= '0;
            ctrl_reset_n    <= 1'b0;
        end else if (!ctrl_reset_n) begin
            if (powerup_counter == POWERUP_HOLD_CYCLES - 1) begin
                ctrl_reset_n <= 1'b1;
            end else begin
                powerup_counter <= powerup_counter + 18'd1;
            end
        end
    end

    always_ff @(posedge ctrl_clk or negedge ctrl_reset_n) begin
        if (!ctrl_reset_n) begin
            init_counter    <= 16'd0;
            global_reset_n  <= 1'b0;
            start_n         <= 1'b1;
        end else begin
            start_n <= 1'b1;
            if (init_counter < 16'd3) begin
                init_counter   <= init_counter + 16'd1;
                global_reset_n <= 1'b0;
            end else if (init_counter < INIT_WAIT_CYCLES) begin
                init_counter   <= init_counter + 16'd1;
                global_reset_n <= 1'b1;
            end else if (init_counter == INIT_WAIT_CYCLES) begin
                init_counter   <= init_counter + 16'd1;
                global_reset_n <= 1'b1;
                start_n        <= 1'b0;
            end else begin
                global_reset_n <= 1'b1;
            end
        end
    end

    Sdram_Control sdram_ctrl (
        .REF_CLK     (clk50),
        .RESET_N     (ctrl_reset_n),
        .CLK         (ctrl_clk),
        .WR_DATA     (wr_data),
        .WR          (wr_req),
        .WR_ADDR     (25'd0),
        .WR_MAX_ADDR (TEST_WORDS_25),
        .WR_LENGTH   (BURST_WORDS_9),
        .WR_LOAD     (!global_reset_n),
        .WR_CLK      (ctrl_clk),
        .WR_FULL     (wr_full),
        .WR_USE      (wr_used),
        .RD_DATA     (rd_data),
        .RD          (rd_req),
        .RD_ADDR     (25'd0),
        .RD_MAX_ADDR (TEST_WORDS_25),
        .RD_LENGTH   (BURST_WORDS_9),
        .RD_LOAD     (!global_reset_n),
        .RD_CLK      (ctrl_clk),
        .RD_EMPTY    (rd_empty),
        .RD_USE      (rd_used),
        .SA          (DRAM_ADDR),
        .BA          (DRAM_BA),
        .CS_N        (dram_cs_n_bus),
        .CKE         (DRAM_CKE),
        .RAS_N       (DRAM_RAS_N),
        .CAS_N       (DRAM_CAS_N),
        .WE_N        (DRAM_WE_N),
        .DQ          (DRAM_DQ),
        .DQM         ({DRAM_UDQM, DRAM_LDQM}),
        .SDR_CLK     (DRAM_CLK)
    );

    assign DRAM_CS_N = dram_cs_n_bus[0];

    RW_Test #(
        .MAX_ADDRESS(TEST_LAST_ADDR_25)
    ) rw_test (
        .iCLK                     (ctrl_clk),
        .iRST_n                   (ctrl_reset_n),
        .iBUTTON                  (start_n),
        .current_address          (current_address),
        .write                    (wr_req),
        .writedata                (wr_data),
        .read                     (rd_req),
        .readdata                 (rd_data),
        .drv_status_pass          (rw_pass),
        .drv_status_fail          (rw_fail),
        .drv_status_test_complete (rw_done),
        .c_state                  (rw_state),
        .same                     (rw_same)
    );

    assign test_state = rw_state;
    assign test_pass = rw_pass;
    assign test_fail = rw_fail;
    assign test_running = ctrl_reset_n && !rw_done;

    always_comb begin
        if (!ctrl_reset_n || !global_reset_n)
            status_code = STATUS_RESET;
        else if (rw_fail)
            status_code = STATUS_FAIL;
        else if (rw_pass)
            status_code = STATUS_PASS;
        else if (rw_state >= 4'd4)
            status_code = STATUS_READ;
        else if (rw_state != 4'd0)
            status_code = STATUS_WRITE;
        else
            status_code = STATUS_RESET;
    end

    always_comb begin
        case (status_code)
            STATUS_WRITE: progress_words = current_address + 25'd1;
            STATUS_READ:  progress_words = current_address + 25'd1;
            STATUS_PASS:  progress_words = TEST_WORDS;
            STATUS_FAIL:  progress_words = current_address;
            default:      progress_words = 32'd0;
        endcase

        if (progress_words >= TEST_WORDS)
            bar_fill_px = BAR_W;
        else
            bar_fill_px = (progress_words * BAR_W) / TEST_WORDS;
    end

    sdram_test_vga_timing timing (
        .clk50       (clk50),
        .reset       (reset_req || !ctrl_reset_n),
        .hcount      (hcount),
        .vcount      (vcount),
        .VGA_CLK     (VGA_CLK),
        .VGA_HS      (VGA_HS),
        .VGA_VS      (VGA_VS),
        .VGA_BLANK_n (VGA_BLANK_N),
        .VGA_SYNC_n  (VGA_SYNC_N)
    );

    assign pixel_x = hcount[10:1];
    assign pixel_y = vcount;
    assign pixel_visible = VGA_BLANK_N && (pixel_x < 10'd640) && (pixel_y < 10'd480);

    always_comb begin
        integer dx;
        integer dy;
        integer char_idx;
        integer glyph_x;
        integer glyph_y;
        logic [7:0] ch;

        dx = 0;
        dy = 0;
        char_idx = 0;
        glyph_x = 0;
        glyph_y = 0;
        ch = " ";
        title_on = 1'b0;
        status_on = 1'b0;
        bar_border_on = 1'b0;
        bar_fill_on = 1'b0;
        bg_rgb = 24'h000000;
        pixel_rgb = 24'h000000;
        glyph_row = 5'b00000;

        case (status_code)
            STATUS_WRITE: bg_rgb = 24'h102A56;
            STATUS_READ:  bg_rgb = 24'h14504A;
            STATUS_PASS:  bg_rgb = 24'h184E1E;
            STATUS_FAIL:  bg_rgb = 24'h5A1818;
            default:      bg_rgb = 24'h202028;
        endcase

        if (pixel_x >= TITLE_X &&
            pixel_x < TITLE_X + TITLE_CHARS * CELL_W * TITLE_SCALE &&
            pixel_y >= TITLE_Y &&
            pixel_y < TITLE_Y + FONT_H * TITLE_SCALE) begin
            dx = pixel_x - TITLE_X;
            dy = pixel_y - TITLE_Y;
            char_idx = dx / (CELL_W * TITLE_SCALE);
            glyph_x = (dx / TITLE_SCALE) % CELL_W;
            glyph_y = dy / TITLE_SCALE;
            if ((char_idx >= 0) && (char_idx < TITLE_CHARS) &&
                (glyph_x >= 0) && (glyph_x < FONT_W) &&
                (glyph_y >= 0) && (glyph_y < FONT_H)) begin
                ch = title_char(char_idx[3:0]);
                glyph_row = glyph_bits(ch, glyph_y[2:0]);
                title_on = glyph_row[FONT_W - 1 - glyph_x];
            end
        end

        if (pixel_x >= STATUS_X &&
            pixel_x < STATUS_X + STATUS_CHARS * CELL_W * STATUS_SCALE &&
            pixel_y >= STATUS_Y &&
            pixel_y < STATUS_Y + FONT_H * STATUS_SCALE) begin
            dx = pixel_x - STATUS_X;
            dy = pixel_y - STATUS_Y;
            char_idx = dx / (CELL_W * STATUS_SCALE);
            glyph_x = (dx / STATUS_SCALE) % CELL_W;
            glyph_y = dy / STATUS_SCALE;
            if ((char_idx >= 0) && (char_idx < STATUS_CHARS) &&
                (glyph_x >= 0) && (glyph_x < FONT_W) &&
                (glyph_y >= 0) && (glyph_y < FONT_H)) begin
                ch = status_char(status_code, char_idx[3:0]);
                glyph_row = glyph_bits(ch, glyph_y[2:0]);
                status_on = glyph_row[FONT_W - 1 - glyph_x];
            end
        end

        if (pixel_x >= BAR_X && pixel_x < BAR_X + BAR_W &&
            pixel_y >= BAR_Y && pixel_y < BAR_Y + BAR_H) begin
            bar_border_on = (pixel_x == BAR_X) ||
                            (pixel_x == BAR_X + BAR_W - 1) ||
                            (pixel_y == BAR_Y) ||
                            (pixel_y == BAR_Y + BAR_H - 1);
            bar_fill_on = (pixel_x < BAR_X + bar_fill_px) &&
                          (pixel_x > BAR_X) &&
                          (pixel_y > BAR_Y) &&
                          (pixel_y < BAR_Y + BAR_H - 1);
        end

        pixel_rgb = bg_rgb;
        if (bar_fill_on)
            pixel_rgb = 24'hF0D060;
        if (bar_border_on)
            pixel_rgb = 24'hFFFFFF;
        if (title_on || status_on)
            pixel_rgb = 24'hFFFFFF;
        if (!pixel_visible)
            pixel_rgb = 24'h000000;
    end

    assign VGA_R = pixel_rgb[23:16];
    assign VGA_G = pixel_rgb[15:8];
    assign VGA_B = pixel_rgb[7:0];

    function automatic [7:0] title_char(input logic [3:0] index);
        begin
            case (index)
                4'd0: title_char = "S";
                4'd1: title_char = "D";
                4'd2: title_char = "R";
                4'd3: title_char = "A";
                4'd4: title_char = "M";
                4'd5: title_char = " ";
                4'd6: title_char = "T";
                4'd7: title_char = "E";
                4'd8: title_char = "S";
                4'd9: title_char = "T";
                default: title_char = " ";
            endcase
        end
    endfunction

    function automatic [7:0] status_char(input status_t status, input logic [3:0] index);
        begin
            case (status)
                STATUS_WRITE: begin
                    case (index)
                        4'd0: status_char = "W";
                        4'd1: status_char = "R";
                        4'd2: status_char = "I";
                        4'd3: status_char = "T";
                        4'd4: status_char = "E";
                        default: status_char = " ";
                    endcase
                end
                STATUS_READ: begin
                    case (index)
                        4'd0: status_char = "R";
                        4'd1: status_char = "E";
                        4'd2: status_char = "A";
                        4'd3: status_char = "D";
                        default: status_char = " ";
                    endcase
                end
                STATUS_PASS: begin
                    case (index)
                        4'd0: status_char = "P";
                        4'd1: status_char = "A";
                        4'd2: status_char = "S";
                        4'd3: status_char = "S";
                        default: status_char = " ";
                    endcase
                end
                STATUS_FAIL: begin
                    case (index)
                        4'd0: status_char = "F";
                        4'd1: status_char = "A";
                        4'd2: status_char = "I";
                        4'd3: status_char = "L";
                        default: status_char = " ";
                    endcase
                end
                default: begin
                    case (index)
                        4'd0: status_char = "R";
                        4'd1: status_char = "E";
                        4'd2: status_char = "S";
                        4'd3: status_char = "E";
                        4'd4: status_char = "T";
                        default: status_char = " ";
                    endcase
                end
            endcase
        end
    endfunction

    function automatic [4:0] glyph_bits(input logic [7:0] ch, input logic [2:0] row);
        begin
            glyph_bits = 5'b00000;
            case (ch)
                "A": begin
                    case (row)
                        3'd0: glyph_bits = 5'b01110;
                        3'd1: glyph_bits = 5'b10001;
                        3'd2: glyph_bits = 5'b10001;
                        3'd3: glyph_bits = 5'b11111;
                        3'd4: glyph_bits = 5'b10001;
                        3'd5: glyph_bits = 5'b10001;
                        3'd6: glyph_bits = 5'b10001;
                        default: glyph_bits = 5'b00000;
                    endcase
                end
                "D": begin
                    case (row)
                        3'd0: glyph_bits = 5'b11110;
                        3'd1: glyph_bits = 5'b10001;
                        3'd2: glyph_bits = 5'b10001;
                        3'd3: glyph_bits = 5'b10001;
                        3'd4: glyph_bits = 5'b10001;
                        3'd5: glyph_bits = 5'b10001;
                        3'd6: glyph_bits = 5'b11110;
                        default: glyph_bits = 5'b00000;
                    endcase
                end
                "E": begin
                    case (row)
                        3'd0: glyph_bits = 5'b11111;
                        3'd1: glyph_bits = 5'b10000;
                        3'd2: glyph_bits = 5'b10000;
                        3'd3: glyph_bits = 5'b11110;
                        3'd4: glyph_bits = 5'b10000;
                        3'd5: glyph_bits = 5'b10000;
                        3'd6: glyph_bits = 5'b11111;
                        default: glyph_bits = 5'b00000;
                    endcase
                end
                "F": begin
                    case (row)
                        3'd0: glyph_bits = 5'b11111;
                        3'd1: glyph_bits = 5'b10000;
                        3'd2: glyph_bits = 5'b10000;
                        3'd3: glyph_bits = 5'b11110;
                        3'd4: glyph_bits = 5'b10000;
                        3'd5: glyph_bits = 5'b10000;
                        3'd6: glyph_bits = 5'b10000;
                        default: glyph_bits = 5'b00000;
                    endcase
                end
                "I": begin
                    case (row)
                        3'd0: glyph_bits = 5'b11111;
                        3'd1: glyph_bits = 5'b00100;
                        3'd2: glyph_bits = 5'b00100;
                        3'd3: glyph_bits = 5'b00100;
                        3'd4: glyph_bits = 5'b00100;
                        3'd5: glyph_bits = 5'b00100;
                        3'd6: glyph_bits = 5'b11111;
                        default: glyph_bits = 5'b00000;
                    endcase
                end
                "L": begin
                    case (row)
                        3'd0: glyph_bits = 5'b10000;
                        3'd1: glyph_bits = 5'b10000;
                        3'd2: glyph_bits = 5'b10000;
                        3'd3: glyph_bits = 5'b10000;
                        3'd4: glyph_bits = 5'b10000;
                        3'd5: glyph_bits = 5'b10000;
                        3'd6: glyph_bits = 5'b11111;
                        default: glyph_bits = 5'b00000;
                    endcase
                end
                "M": begin
                    case (row)
                        3'd0: glyph_bits = 5'b10001;
                        3'd1: glyph_bits = 5'b11011;
                        3'd2: glyph_bits = 5'b10101;
                        3'd3: glyph_bits = 5'b10101;
                        3'd4: glyph_bits = 5'b10001;
                        3'd5: glyph_bits = 5'b10001;
                        3'd6: glyph_bits = 5'b10001;
                        default: glyph_bits = 5'b00000;
                    endcase
                end
                "P": begin
                    case (row)
                        3'd0: glyph_bits = 5'b11110;
                        3'd1: glyph_bits = 5'b10001;
                        3'd2: glyph_bits = 5'b10001;
                        3'd3: glyph_bits = 5'b11110;
                        3'd4: glyph_bits = 5'b10000;
                        3'd5: glyph_bits = 5'b10000;
                        3'd6: glyph_bits = 5'b10000;
                        default: glyph_bits = 5'b00000;
                    endcase
                end
                "R": begin
                    case (row)
                        3'd0: glyph_bits = 5'b11110;
                        3'd1: glyph_bits = 5'b10001;
                        3'd2: glyph_bits = 5'b10001;
                        3'd3: glyph_bits = 5'b11110;
                        3'd4: glyph_bits = 5'b10100;
                        3'd5: glyph_bits = 5'b10010;
                        3'd6: glyph_bits = 5'b10001;
                        default: glyph_bits = 5'b00000;
                    endcase
                end
                "S": begin
                    case (row)
                        3'd0: glyph_bits = 5'b01111;
                        3'd1: glyph_bits = 5'b10000;
                        3'd2: glyph_bits = 5'b10000;
                        3'd3: glyph_bits = 5'b01110;
                        3'd4: glyph_bits = 5'b00001;
                        3'd5: glyph_bits = 5'b00001;
                        3'd6: glyph_bits = 5'b11110;
                        default: glyph_bits = 5'b00000;
                    endcase
                end
                "T": begin
                    case (row)
                        3'd0: glyph_bits = 5'b11111;
                        3'd1: glyph_bits = 5'b00100;
                        3'd2: glyph_bits = 5'b00100;
                        3'd3: glyph_bits = 5'b00100;
                        3'd4: glyph_bits = 5'b00100;
                        3'd5: glyph_bits = 5'b00100;
                        3'd6: glyph_bits = 5'b00100;
                        default: glyph_bits = 5'b00000;
                    endcase
                end
                "W": begin
                    case (row)
                        3'd0: glyph_bits = 5'b10001;
                        3'd1: glyph_bits = 5'b10001;
                        3'd2: glyph_bits = 5'b10001;
                        3'd3: glyph_bits = 5'b10101;
                        3'd4: glyph_bits = 5'b10101;
                        3'd5: glyph_bits = 5'b10101;
                        3'd6: glyph_bits = 5'b01010;
                        default: glyph_bits = 5'b00000;
                    endcase
                end
                " ": glyph_bits = 5'b00000;
                default: glyph_bits = 5'b00000;
            endcase
        end
    endfunction

endmodule

module sdram_test_vga_timing (
    input  logic        clk50,
    input  logic        reset,
    output logic [10:0] hcount,
    output logic  [9:0] vcount,
    output logic        VGA_CLK,
    output logic        VGA_HS,
    output logic        VGA_VS,
    output logic        VGA_BLANK_n,
    output logic        VGA_SYNC_n
);

    parameter HACTIVE      = 11'd1280;
    parameter HFRONT_PORCH = 11'd32;
    parameter HSYNC        = 11'd192;
    parameter HBACK_PORCH  = 11'd96;
    parameter HTOTAL       = HACTIVE + HFRONT_PORCH + HSYNC + HBACK_PORCH;

    parameter VACTIVE      = 10'd480;
    parameter VFRONT_PORCH = 10'd10;
    parameter VSYNC        = 10'd2;
    parameter VBACK_PORCH  = 10'd33;
    parameter VTOTAL       = VACTIVE + VFRONT_PORCH + VSYNC + VBACK_PORCH;

    logic end_of_line;
    logic end_of_field;

    always_ff @(posedge clk50) begin
        if (reset)
            hcount <= 11'd0;
        else if (end_of_line)
            hcount <= 11'd0;
        else
            hcount <= hcount + 11'd1;
    end

    assign end_of_line = (hcount == HTOTAL - 11'd1);

    always_ff @(posedge clk50) begin
        if (reset)
            vcount <= 10'd0;
        else if (end_of_line) begin
            if (end_of_field)
                vcount <= 10'd0;
            else
                vcount <= vcount + 10'd1;
        end
    end

    assign end_of_field = (vcount == VTOTAL - 10'd1);

    assign VGA_HS = !((hcount[10:8] == 3'b101) & !(hcount[7:5] == 3'b111));
    assign VGA_VS = !(vcount[9:1] == 9'd245);
    assign VGA_SYNC_n  = 1'b0;
    assign VGA_BLANK_n = !(hcount[10] & (hcount[9] | hcount[8])) &
                         !(vcount[9] | (vcount[8:5] == 4'b1111));
    assign VGA_CLK = hcount[0];

endmodule

`include "sdram_controller/Sdram_Control.v"
`include "sdram_controller/control_interface.v"
`include "sdram_controller/command.v"
`include "sdram_controller/sdr_data_path.v"
`include "sdram_controller/Sdram_RD_FIFO.v"
`include "sdram_controller/Sdram_WR_FIFO.v"
`include "sdram_controller/sdram_pll0.v"
`include "sdram_controller/sdram_pll0/sdram_pll0_0002.v"
`include "sdram_local_test/RW_Test.v"
