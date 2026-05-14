// Per-frame performance counters for the voxel GPU.

module voxel_perf_counters #(
    parameter [8:0] COPY_WR_FIFO_HIGH_WATER = 9'd224
) (
    input  logic        clk,
    input  logic        reset,
    input  logic        flip_write,

    input  logic        in_draw,
    input  logic        draw_commit,
    input  logic        in_cache_init,

    input  logic        flush_active,
    input  logic        flush_push,
    input  logic        flush_load_pending,
    input  logic        sdram_wr_full,
    input  logic [15:0] sdram_wr_use,
    input  logic [15:0] flush_words_done,
    input  logic [15:0] flush_pixels_total,
    input  logic        flush_word_pending_valid,
    input  logic        flush_fetch_inflight,

    output logic [31:0] perf_draw_active,
    output logic [31:0] perf_draw_idle,
    output logic [31:0] perf_flush_active,
    output logic [31:0] perf_flush_stall,
    output logic [31:0] perf_init,
    output logic [31:0] perf_load,
    output logic [31:0] perf_flush_wait_load,
    output logic [31:0] perf_flush_wait_fifo,
    output logic [31:0] perf_flush_wait_data,
    output logic [31:0] perf_flush_wait_drain
);
    wire flush_stalling = flush_active && !flush_push;
    wire flush_done_wait =
        (flush_words_done == flush_pixels_total) &&
        !flush_word_pending_valid &&
        !flush_fetch_inflight;
    wire flush_fifo_wait =
        sdram_wr_full || (sdram_wr_use[8:0] >= COPY_WR_FIFO_HIGH_WATER);

    always_ff @(posedge clk) begin
        if (reset || flip_write) begin
            perf_draw_active  <= 32'd0;
            perf_draw_idle    <= 32'd0;
            perf_flush_active <= 32'd0;
            perf_flush_stall  <= 32'd0;
            perf_init         <= 32'd0;
            perf_load         <= 32'd0;
            perf_flush_wait_load  <= 32'd0;
            perf_flush_wait_fifo  <= 32'd0;
            perf_flush_wait_data  <= 32'd0;
            perf_flush_wait_drain <= 32'd0;
        end else begin
            if (in_draw && draw_commit)
                perf_draw_active <= perf_draw_active + 32'd1;
            if (in_draw && !draw_commit)
                perf_draw_idle <= perf_draw_idle + 32'd1;
            if (flush_active && flush_push)
                perf_flush_active <= perf_flush_active + 32'd1;
            if (flush_active && !flush_push)
                perf_flush_stall <= perf_flush_stall + 32'd1;
            if (flush_stalling && flush_load_pending)
                perf_flush_wait_load <= perf_flush_wait_load + 32'd1;
            else if (flush_stalling && flush_fifo_wait)
                perf_flush_wait_fifo <= perf_flush_wait_fifo + 32'd1;
            else if (flush_stalling && flush_done_wait)
                perf_flush_wait_drain <= perf_flush_wait_drain + 32'd1;
            else if (flush_stalling && !flush_word_pending_valid)
                perf_flush_wait_data <= perf_flush_wait_data + 32'd1;
            if (in_cache_init)
                perf_init <= perf_init + 32'd1;
        end
    end
endmodule
