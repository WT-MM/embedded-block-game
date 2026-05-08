Project Notes
=============

May 2026: SDRAM Banding Bring-Up Ledger
---------------------------------------

Summary
-------
This section consolidates the recent SDRAM-backed 640x480 bring-up work. The
symptoms were frustrating because several different bugs produced similar
"streaking/flashing" output:

  * a solid-colored bottom band, initially described as the bottom row
  * short one-pixel-tall horizontal streaks near the left edge
  * a one-pixel bad column on the left, as if the rightmost column wrapped
  * a right-edge/left-shift symptom where the FPS text and chat `M` were cut off
  * rapid sky/ground flashing, while block-heavy views were much more stable
  * frame rate falling from the software 30 FPS cap to roughly 5-20 FPS

The important pattern from hardware testing was that views dominated by sky or
ground are the dangerous case. Those views generate very little real raster
work, so software reaches `END_BAND`, `FLIP`, and the next `CLEAR_FRAME` much
faster. That fast-frame path stresses SDRAM scanout/flush ordering in ways that
ordinary block views naturally hide by keeping the rasterizer busy.

Current Architecture
--------------------
The current full-resolution path renders eight 60-line vertical bands:

  * userspace still builds one frame descriptor stream in `sw/renderer.c`
  * `sw/gpu_transport.c` bins descriptors by band and wraps each bin with
    `BEGIN_BAND` / descriptor writes / `END_BAND`
  * the RTL has two local color/Z band caches, so it can draw into one cache
    while a previous band flushes from the other cache to SDRAM
  * VGA scanout reads the visible SDRAM frame through three 640-word line
    buffers
  * the SDRAM controller is FIFO/burst based, so most bugs are ownership,
    ordering, or stale-tail issues rather than simple address arithmetic

Bottom Solid Band
-----------------
Symptom:

  * The bottom of the display was a solid-colored band, not merely one bad row.
  * The color changed with camera movement, which made it look like stale cache
    data or a repeated pixel rather than a fixed VGA timing problem.

Root cause:

  * The final band has no follow-up `BEGIN_BAND` to toggle `draw_cache_sel` away
    from the cache being flushed.
  * The ping-pong cache port mux was still giving the rasterizer side priority
    even after the rasterizer/cache-maintenance pipeline was idle.
  * The flush controller therefore read through the wrong port/address and could
    stream a repeated stale pixel into SDRAM for the whole final band.

Fix:

  * `cache_used_by_main` now means "the rasterizer, sky-skip patch, cache init,
    cache load, or main cache flush is actually using the active cache port."
  * Once the pipeline is idle, the background flush is allowed to own the cache
    port even if `flush_cache_sel == draw_cache_sel`.
  * `band_pixel_count()` also uses 10-bit row math so the final band is treated
    as rows 420..479 instead of wrapping the row calculation in 9 bits.

Status:

  * The solid bottom band was fixed on hardware after the relevant band-pass
    fixes were included.

Left-Edge Streaks and Wrapped Column
------------------------------------
Symptoms:

  * Early versions produced short one-pixel-tall horizontal streak fragments
    from the left edge, often around the first 1/10 of the screen width.
  * A later version reduced that to a one-pixel bad left column, visually like
    the rightmost pixel of a line had wrapped to column 0.

Root causes and fixes:

  * The scanout line-fill path could pop the SDRAM read FIFO while
    `scan_fill_armed` was still true. In that state a new RD_LOAD had just been
    programmed, but the new burst had not produced data yet. Any non-empty FIFO
    word was stale tail data from a previous burst. The fix gates scanout pops
    with `!scan_fill_armed`.
  * Cache color/Z loads can over-fetch because the SDRAM controller bursts in
    64-word chunks. The explicit drain states pop residual read-FIFO words and
    wait for `sdram_rd_empty` to be stable before the next read owner is allowed
    to start.
  * As a defense-in-depth display mask, when a scanline finishes filling,
    `scan_linebufN[0]` is overwritten with `scan_linebufN[1]`. This does not
    fix a corrupted framebuffer word; it masks the common stale first read word
    in the linebuffer so the leftmost visible column does not show wrapped tail
    data.

Wrong turns:

  * Waiting a fixed number of cycles before trusting `RD_EMPTY` did not solve
    the issue; stale words can simply sit in the FIFO until the wait expires.
  * Software-side "band primer" quads did not fix the original left-edge
    streaking. They were useful as a diagnostic but were not the root cause.

Status:

  * The large left-edge streaks were fixed.
  * The one-pixel left column has been mitigated by the linebuffer guard, but if
    it reappears it should still be treated as a scanout read-FIFO first-word
    problem before chasing world/texture code.

Horizontal Shift / Cut-Off Text
-------------------------------
Symptoms:

  * The FPS label looked shifted/cut so only `PS` was visible.
  * Opening chat could show the `M` in a mode/status string partially cut off.
  * Later testing reported that the gross horizontal shift was gone, but the
    one-pixel left-column artifact remained.

Fixes in the current RTL:

  * Scanout uses a combinational visible predicate for `scan_visible_now` so the
    RGB data and `VGA_BLANK_n` phase line up at the DAC instead of using a
    stale registered visible bit.
  * Scanline handoff only changes the active linebuffer in horizontal blank.
  * The column-0 linebuffer guard described above masks the remaining first-word
    FIFO-tail case without shifting the whole image.

Status:

  * The broad horizontal shift/cut-off symptom was reported fixed.
  * Any remaining single-column issue is tracked under "Left-Edge Streaks and
    Wrapped Column" above.

Sky Gradient / Empty-Band Flushes
---------------------------------
Symptom:

  * Sky and ground views were visually unstable and slow because they contain
    many bands with little or no real raster work.
  * The software sky gradient originally arrived as large flat quads; after
    banding, those quads cost descriptor traffic and made empty-band behavior
    harder to reason about.

Fix:

  * Hardware now has `VOXEL_EXTMEM_CTRL_SKY_GRADIENT_CLEAR`.
  * `ST_CACHE_INIT` can initialize each local band cache directly to the correct
    24-row sky-gradient palette sequence (`PAL_SKY_GRADIENT_BASE..LAST`) while
    also clearing Z to far.
  * Redundant full-width sky-gradient descriptors are skipped in `ST_FETCH`.
  * A one-pixel sky patch exists for skipped sky descriptors so a skipped
    descriptor can still mark/patch the relevant cache location when needed.
  * `cache_dirty` is set after sky-gradient initialization so sky-only bands
    still flush meaningful pixels to SDRAM.

Performance follow-up:

  * `cache_draw_dirty` tracks whether a band had any real draw commits.
  * The 1x1 band primer is explicitly excluded from `cache_draw_dirty`.
  * If a band only contains generated sky/clear data, the background flush can
    generate the sky-gradient RGB565 stream directly from palette entries
    instead of reading from a local cache. This lets the next fast band reuse
    either local cache without waiting for a generated-sky flush to release a
    cache read port.

Status:

  * This improves the fast sky-only path and avoids treating primers as real
    scene content.
  * It was not the final visual fix for sky/ground flashing; the final fix was
    preserving in-flight flush/scanout state across `CLEAR_FRAME` plus the
    queued flush/begin ordering fixes below.

Band Flush / Begin Ordering
---------------------------
Problem:

  * Once `BEGIN_BAND` and `END_BAND` stopped waiting for every background flush,
    userspace could pipeline commands much more aggressively.
  * That exposed ordering races between `band_flush_pending`,
    `band_begin_pending`, descriptor FIFO fetch, and `cache_band_index`.

Fixes:

  * `band_begin_cache_available` prevents a new begin from toggling into a cache
    that is still being flushed.
  * `band_begin_cache_available` also checks `!band_flush_pending`, because a
    queued flush must be captured into its own `flush_*` registers before a
    later begin is allowed to overwrite `cache_band_index`.
  * The `ST_IDLE` priority chain now lets a blocked begin fall through so a
    queued flush can run first.
  * `ST_FETCH` is gated by `!band_flush_pending`, so descriptors for band N+1
    cannot be fetched into the cache for band N while the old end-band flush is
    still queued.
  * A temporary `fifo_count == 0` gate on the flush branch caused a deadlock:
    the flush was waiting for the FIFO to empty, while fetch was correctly
    waiting for the flush to clear `band_flush_pending`. That gate was removed.
    This is safe because `end_band()` polls FIFO-empty before setting
    `band_flush_pending`; any later FIFO contents belong to the next band and
    are blocked by the `ST_FETCH` gate.

Status:

  * These fixes address the "fast software outruns RTL bookkeeping" class of
    bugs introduced by the performance work.

Kernel/Driver Throughput Fixes
------------------------------
Problems:

  * The driver used to push descriptor words in tiny increments when the FIFO
    was nearly full.
  * `BEGIN_BAND` waited for band cache init to finish before userspace could
    stream descriptors.
  * `END_BAND` waited for background flush completion, serializing every band.

Fixes:

  * `voxel_write()` waits for useful FIFO space and then pushes bursts with
    `iowrite32_rep()`.
  * `BEGIN_BAND` now writes `BAND_INDEX` / `BAND_CTRL_BEGIN`, performs a readback
    for MMIO ordering, and returns without waiting for the whole cache init.
    The descriptor FIFO can fill while the hardware initializes the band.
  * `band_flush_pending` is intentionally excluded from `BSY`.
  * `END_BAND` no longer waits for background flush completion. The RTL
    priority chain and cache-availability gates own that ordering.

Tradeoff:

  * These changes are required for useful frame rate, but they expose any RTL
    ordering bug immediately. Several of the fixes in "Band Flush / Begin
    Ordering" were direct consequences of making the driver less conservative.

Sky/Ground Flashing
-------------------
Final symptom before the fix:

  * Hardware was stable when looking at blocks, but flashed or vertically
    streaked when looking mostly at sky or ground.
  * This was the key clue: block-heavy views naturally keep the rasterizer busy,
    while sky/ground views finish bands extremely quickly and stress
    `END_BAND`/`BEGIN_BAND`/`CLEAR_FRAME` ordering.

What actually fixed it:

  * **Preserve background flushes across `CLEAR_FRAME`.** The decisive fix was
    to stop clearing `flush_active` and the associated `flush_*` state on
    `CLEAR_FRAME`. Fast sky/ground frames can issue the next frame's clear while
    the previous frame's background band flush is still draining. Killing that
    flush mid-stream left a partially-written SDRAM frame, which scanout later
    displayed as a flash.
  * **Preserve the scanout read pipeline across `CLEAR_FRAME`.** The same clear
    path also stopped resetting `sdram_rd_load_pulse`,
    `sdram_rd_load_stretch_req`, `sdram_rd_load_hold`, `scan_rd_capture`, and
    `scan_fill_load_pending`. `CLEAR_FRAME` is a software frame-begin command,
    not a display reset; with the 30 FPS cap it can land during active display.
    Resetting scanout RD/load state mid-line corrupts the current linebuffer
    fill.
  * **Order queued flushes before queued begins.** Once `END_BAND` stopped
    waiting for every background flush, software could pipeline commands fast
    enough to expose RTL bookkeeping races. `band_begin_cache_available` now
    requires `!band_flush_pending`, and the `ST_IDLE` priority chain lets a
    queued flush capture its own `flush_*` registers before a later begin can
    overwrite `cache_band_index`.
  * **Block next-band descriptor fetch while a prior flush is queued.**
    `ST_FETCH` is gated by `!band_flush_pending`, so descriptors for band N+1
    cannot be drawn into band N's cache while the previous `END_BAND` is still
    queued.
  * **Remove the false FIFO-empty flush gate.** A temporary
    `fifo_count == 0` requirement on the flush branch deadlocked the correct
    ordering: fetch was waiting for `band_flush_pending` to clear, while the
    flush was waiting for the next band's FIFO contents to drain. Removing that
    gate let the queued flush run; next-band descriptors remain protected by
    the `ST_FETCH` gate.

What did not fix it by itself:

  * The generated-sky direct flush path was a performance optimization, not the
    root visual fix.
  * The sky-gradient hardware clear removed redundant sky descriptors and made
    empty bands meaningful, but did not by itself eliminate flashing.
  * Fixed delay guards around read-FIFO empty/stale data helped left-edge
    residuals, but sky/ground flashing was mostly a frame-clear/background-flush
    ordering problem.

Status:

  * Confirmed fixed on hardware after the `CLEAR_FRAME` preservation and
    queued flush/begin ordering fixes were included.
  * The remaining problem is throughput, not visual correctness.

Frame Rate Status
-----------------
Observed behavior:

  * The software cap is 30 FPS.
  * After the early correctness fixes, measured frame rate moved through roughly
    5 FPS, 15 FPS, and about 20 FPS depending on which RTL/driver changes were
    included.
  * `renderer_end_frame()` / kernel waiting was the dominant cost when the GPU
    was slow; CPU descriptor work was not the only bottleneck.

Main fixes:

  * Kernel FIFO burst submission.
  * Removing redundant driver waits around `BEGIN_BAND` and `END_BAND`.
  * Ping-pong local band caches with background flush.
  * Hardware sky-gradient clear and redundant sky-descriptor skip.
  * Generated-sky direct flush for sky-only bands.

Open performance risk:

  * Visual correctness is now fixed on hardware. Further throughput work should
    preserve the `CLEAR_FRAME` and queued flush/begin ordering rules above;
    otherwise the old sky/ground flash can come back even if FPS improves.

Validation Notes
----------------
Local validation that is safe on the development machine:

  * `git diff --check`
  * Verilator lint of `hw/voxel_gpu/rtl/voxel_gpu.sv` with the existing vendor
    primitive warnings suppressed
  * software renderer/unit test builds that do not require Linux-only headers

Local validation limits:

  * Do not rely on Quartus from this machine; it is not installed here.
  * The `game` target does not build on the Mac because `linux/input.h` is not
    available.
  * Any future RTL change that touches scanout, flush, or band ordering still
    requires flashing and visual testing on the DE1-SoC.

May 2026: Frame-Rate Optimization Pass
--------------------------------------

Summary
-------
We investigated a frame-rate bottleneck report that pointed at three likely
hot spots:

  * kernel FIFO submission degenerating into one-word AXI writes
  * userspace descriptor packing spending too much time in libc rounding calls
  * userspace opaque-quad sorting/copying duplicating work already handled by
    the hardware banding transport path

The report was directionally correct, but the banding recommendation needed to
be adapted to this codebase: `sw/gpu_transport.c` already performs the actual
per-band descriptor binning/clipping before issuing `BEGIN_BAND`/`END_BAND`.
Adding another set of band buffers inside `RenderContext` would have duplicated
that ownership. The cleaner fix was to remove the redundant renderer-side
opaque sort and keep transport as the single place that understands hardware
band submission.

Work Completed
--------------
Kernel FIFO streaming in `sw/voxel_gpu.c` now waits for burst-sized FIFO space
before writing descriptors. The old loop resumed as soon as *one* FIFO word was
free, which could collapse into repeated AXI status reads and tiny one-word
writes when software outran the rasterizer. The new path waits for 64 words
unless fewer words remain in the current write chunk, then pushes the whole
available burst with `iowrite32_rep()`. The wait loop now uses
`usleep_range(10, 50)` instead of a tight `udelay()` spin, and defensively
clamps bogus FIFO-count reads above the FIFO depth to zero available space.

Descriptor fixed-point packing in `sw/renderer.c` no longer calls `roundf()` or
`lroundf()` in the hot per-quad path. The Q24.8, Q1.15, Q8.8, Q0.16, and Q16.16
packers now use bounded multiply-plus-cast rounding. This keeps the descriptor
setup loop from paying libc math-call overhead for every edge/depth/UV
coefficient.

The inclusive bbox calculation in `stage_prepared_quad()` now uses integer
Q24.8 math after screen-space snapping instead of `ceilf()`/`floorf()`. A local
sanity check verified that the integer formulas match the previous float
formulas over the tested fixed-point range.

The old `sort_opaque_quads()` path was removed from `sw/renderer.c`. That code
allocated/sized descriptor refs, ran `qsort()`, copied descriptor bytes into a
scratch buffer, then copied them back into `submit_buffer`. Since
`gpu_transport_submit_descriptors()` already bins descriptors by hardware band,
the sort was redundant O(N log N) work plus extra memory traffic.

Transport-side hardware binning in `sw/gpu_transport.c` now grows each band's
flat descriptor buffer independently. Previously, `bins_ensure_flat_capacity()`
allocated enough space for the entire frame's descriptor stream for *every*
band, even though most frames distribute descriptors across bands. Per-band
capacity reduces memory footprint and cache pressure while preserving the same
hardware submission protocol.

Normal-frame timing syscalls in `sw/gpu_transport.c` were also gated behind
`VOXEL_DIAG_BBOX`. `clock_gettime()` remains available for diagnostics, but
the regular submit/flip path no longer pays those syscall costs every frame.

Validation
----------
The software renderer targets rebuilt successfully after the optimization pass:

  * `tests/renderer_scene_test`
  * `tests/renderer_static_test`
  * `tests/renderer_quad_test`
  * `tests/renderer_edge_test`
  * `tests/renderer_fog_test`

`git diff --check` passed.

`tests/renderer_quad_test` ran successfully against `virtualhw` using the socket
backend.

Known local validation limits:

  * The `game` target does not compile on the development Mac because the local
    SDK does not provide `linux/input.h`; this is an environment issue, not a
    regression from the optimization pass.
  * `tests/world_chunk_test` currently fails with
    `lamp face was not self-lit`; that failure predates and appears unrelated
    to this FPS work.

Future Work
-----------
Measure the hardware impact with `VOXEL_DIAG_BBOX=1` and external frame timing
on the DE1-SoC. The changes target CPU/AXI overhead, but the real win should be
confirmed on the HPS/FPGA pair rather than inferred from desktop builds.

Consider moving hardware band binning earlier, directly into renderer
submission, so descriptors are never first staged as one giant frame stream and
then re-binned in `gpu_transport.c`. This is a larger ownership change than the
safe transport cleanup above. It should only be done if profiling still shows
descriptor copying/binning as a meaningful bottleneck.

Audit the remaining non-hot `lroundf()` calls in `sw/renderer.c`. Most are
palette/light setup or block lookup and are much less urgent than descriptor
packing, but they could be converted to local fast-round helpers if profiling
shows they matter.

Review `renderer_draw_chunk()` for ad hoc block-array rendering. It still
builds a hash lookup every call. The main game path uses premeshed world chunks,
so this is probably not the primary FPS issue, but tests or debug scenes that
call `renderer_draw_chunk()` heavily may benefit from caching or avoiding the
lookup rebuild.

Investigate whether the per-band primer quads in `gpu_transport.c` are still
needed after the palette-pipeline fixes. They are currently disabled/unreferenced
and generate an unused-function warning. If the left-edge stale-palette issue is
confirmed gone on hardware, remove the dead primer path; otherwise wire it back
in intentionally and document the cost.


April 2026: Rasterizer Edge-Function Signedness Bug
---------------------------------------------------

Summary
-------
We found a major RTL bug in the quad rasterizer that made the renderer look
"2.5D" even when the camera math was mostly correct. The most visible symptoms
were:

  * top faces appearing too large when the camera looked down on blocks
  * shared top/side boundaries shifting as the camera rotated
  * slanted quads in the screen-space edge test collapsing toward their
    bounding boxes instead of keeping clean diagonal edges
  * only some edges of a quad appearing to clip correctly

Root Cause
----------
The bug was in `hw/voxel_gpu/rtl/voxel_gpu.sv` inside the edge-function
evaluation logic.
The older code formed the constant term with a manual concatenation:

    {{32{edge_C[i][31]}}, edge_C[i]}

Even though that expression visually looks like a sign extension, the
concatenation operator itself is an unsigned expression. In practice, that
allowed the larger edge-function expression to be evaluated incorrectly for
negative coefficients.

This was especially damaging because the rasterizer's inclusion test is:

    edge_eval >= 0

If an edge coefficient that should be negative gets treated as a huge positive
value, that edge stops clipping the quad properly and the rasterizer fills too
much of the bounding box.

We confirmed the issue with a small Verilator repro. For one negative test case,
the old-style expression produced:

    429496726593

while the correct signed result was:

    -3007

That mismatch exactly matches the observed hardware behavior: quads whose
negative edge terms should have excluded pixels instead admitted them instead.

Fix
---
We rewrote the edge-function path so the full expression stays explicitly in
signed 64-bit arithmetic. The current implementation in
`hw/voxel_gpu/rtl/voxel_gpu.sv`
creates signed intermediate wires for:

  * `edge_A[i] * draw_x_s`
  * `edge_B[i] * draw_y_s`
  * the sign-extended `edge_C[i]`

and then sums those signed intermediate terms.

This avoids mixed signed/unsigned coercion inside a single inline expression and
makes the intent obvious to both simulation tools and Quartus synthesis.

Why This Mattered Visually
--------------------------
This bug explained several confusing rendering symptoms that initially looked
like projection or camera-transform errors:

  * top faces warping as the camera yaw changed
  * a column's top appearing to grow too large from certain angles
  * neighboring faces stealing area from each other
  * screen-space quads in the edge test looking correct on one side but boxy on
    the others

Because the rasterizer was not respecting all four edges consistently, the
monitor output looked like perspective math was broken even though the deeper
issue was actually in coverage testing inside the RTL.

Validation
----------
We validated the fix with three levels of testing:

  1. A direct Verilator repro for the signed/unsigned edge expression.
  2. `sw/tests/renderer_edge_test`, which submits hand-placed screen-space
     quads with strong diagonal edges.
  3. The real block-scene renderer and interactive game camera.

Before the fix, the edge test produced quads that visibly collapsed toward their
bounding boxes and shared edges were wrong. After the fix, the edge ownership
behavior matched the intended geometry and the scene stopped exhibiting the same
top-face growth and edge warping.

Good Report Wording
-------------------
Suggested writeup text for the final report:

  We diagnosed a rasterizer bug caused by signed/unsigned coercion in the
  SystemVerilog edge-function evaluation path. Negative edge coefficients were
  not preserved correctly in the original expression, so some edges failed to
  reject pixels and the rasterizer overfilled the quad bounding box. Visually,
  this made top faces appear too large, caused shared edges to shift during
  camera rotation, and made the renderer look geometrically incorrect even when
  the camera transform was mostly sound. We fixed the issue by keeping the full
  edge equation in explicit signed 64-bit arithmetic and validated the result
  with both synthetic screen-space tests and the full block-scene renderer.

Engineering Takeaway
--------------------
For arithmetic-heavy RTL, especially rasterizers, do not rely on implicit
signedness through large inline expressions. Use explicitly signed intermediate
wires for each term of the equation, then combine them in a final signed add.


April 2026: Palette M10K Async-Read Bug (Red/Green Fringes on Textured Blocks)
------------------------------------------------------------------------------

Summary
-------
After migrating the back buffer to 16-bit RGB565 (commit `527cf03` "16 bit
bram back buffer"), textured blocks — especially stone — developed thin
red/green colored fringes at texel boundaries. The artifact was stable across
camera motion (so it was not a rasterizer coverage bug), matched texel
boundaries exactly, and only appeared on faces that went through the
palette-indexed texturing path. The palette itself was correct, the texture
atlas was correct, and the software-side quad descriptors were correct.

Root Cause
----------
In `hw/voxel_gpu/rtl/voxel_gpu.sv`, the palette storage was declared with

    (* ramstyle = "M10K" *) logic [23:0] palette [0:255];

Prior to `527cf03`, the palette was only ever read from inside clocked
assignments (e.g. the SDRAM copy path latched `palette[fb_back_rd_data]`
into `copy_palette_rgb` during the `ST_COPY` state). That matches Cyclone V
M10K semantics cleanly: M10K blocks require a *synchronous* read port.

The 16-bit back-buffer rework moved the palette lookup into a combinational
expression that feeds the fog-stage input:  

    wire [7:0]  palette_src_addr = (state == ST_CLEAR) ? 8'd0 : draw_pipe_color;
    wire [15:0] palette_src_rgb565 = rgb888_to_rgb565(palette[palette_src_addr]);
    wire [15:0] fog_rgb565         = rgb888_to_rgb565(palette[fog_color]);

That is an *asynchronous* read of an M10K-tagged memory. Quartus handles the
conflict by either absorbing the downstream `fog0_src_rgb565` register into
the M10K output register (which silently adds one cycle of latency to the
palette value only, skewing it relative to every other `fog0_*` signal) or
by demoting storage to logic cells with awkward timing. Either way, under
timing pressure the effective palette address glitches at texel boundaries.

Why Stone Showed It Loudest
---------------------------
Stone's texture tile references palette indices `0x04`, `0x17`, and `0x18`
(all grays). The palette neighbors are:

  * `0x06` = red (`0xFF4040`, block fault color)
  * `0x11` = grass dark green (`0x4F782D`)
  * `0x12` = grass highlight green (`0x84BA57`)

A single-bit glitch on the palette address from `0x04` → `0x06` or from
`0x17`/`0x18` → `0x11`/`0x12` reads out exactly those saturated red and
green entries, which is why the fringes were so vividly red/green instead
of just a muted discoloration.

Fix (applied)
-------------
Change the palette's `ramstyle` from `"M10K"` to `"MLAB"`. MLAB (distributed)
RAM supports asynchronous reads natively, and 256 × 24 bits = 6144 bits fits
comfortably in a handful of MLAB cells. The palette array now lives in MLAB
at `hw/voxel_gpu/rtl/voxel_gpu.sv`:

    (* ramstyle = "MLAB" *) logic [23:0] palette [0:255];

This is a one-line change with no pipeline-depth impact and no latency skew.

Backup Fix (Pipelined Read) — Only If MLAB Is Insufficient
----------------------------------------------------------
If MLAB ever proves insufficient (e.g. MLAB pressure elsewhere, or timing
still marginal), the correctness-preserving alternative is to keep M10K and
make the palette read *genuinely* synchronous by inserting one new pipeline
stage between `draw_pipe` and `fog0`. That stage would:

  1. Register `palette[palette_src_addr]` and `palette[fog_color]` into new
     `pal_rd_*` flops (which Quartus will absorb into the M10K output
     register cleanly).
  2. Forward every other `draw_pipe_*` signal that `fog0` currently consumes
     through a matching set of new flops so all `fog0_*` fields arrive
     together.
  3. Bump `DRAW_FLUSH_CYCLES` from 12 to 13 so the end-of-frame drain waits
     the extra cycle.
  4. Adjust any status/backpressure logic that counts pipeline depth.

This is strictly more work than the MLAB fix and adds a cycle of latency
per pixel (harmless at our throughput, but non-zero). We only need it if
the MLAB solution regresses timing or floorplanning; otherwise MLAB is the
right answer.

Recommendation: **do not** adopt the pipelined fix unless MLAB demonstrably
fails. The MLAB change is localized, minimal, and matches the natural
shape of the expression ("I want the palette value *this cycle*, combinational
in draw_pipe_color"). The pipelined fix is only worth it if we need M10K
capacity back.

Engineering Takeaway
--------------------
An `(* ramstyle = "M10K" *)` array is a **synchronous-read memory**. Reading
it combinationally is not just a timing warning — it is a correctness bug,
because Quartus' workaround (absorbing the downstream register, or demoting
to logic) produces behavior that does not match either a clean async read or
a clean sync read. The rule: if you want combinational reads, declare the
memory as MLAB (or leave it unconstrained so Quartus picks LCs); if you want
M10K, the read must happen inside a clocked `always_ff` block.

Follow-up: Pipelined Palette Read Applied
-----------------------------------------
After flashing the MLAB version of the palette, residual edge fringing was
still visible in practice, so the "Backup Fix (Pipelined Read)" described
above has been applied as well. The palette is now:

  * stored as MLAB (async-read capable), AND
  * read into a new intermediate pipeline stage `plr_*` that lives between
    `draw_pipe` and `fog0`. `plr_src_rgb` and `plr_fog_rgb` are the
    explicitly-registered palette outputs; every other `draw_pipe_*` field
    that `fog0` consumes is forwarded through a matching `plr_*` flop so
    they all arrive at `fog0` on the same cycle.

`DRAW_FLUSH_CYCLES` was bumped from 12 to 13 to account for the extra
pipeline stage, and `pal_valid` is invalidated alongside the other
`*_valid` registers in ST_IDLE / ST_FETCH.

The prefix `plr_` ("palette-lookup register stage") was chosen to avoid
colliding with the existing CSR-side `pal_addr` register used to
auto-increment palette writes from software. The `ST_CLEAR` fast path
still does a direct combinational read of `palette[0]` via a small
`clear_rgb565` wire; the pipelined stage is only in the path for live
rasterization.

Cost: +1 cycle of end-to-end latency per pixel (harmless at our throughput),
a handful of flops, and no change to the software-visible interface.

Follow-up: Two-Stage Palette Read (pal_rd + plr)
------------------------------------------------
After reflashing the MLAB + single-plr version, chromatic fringing was STILL
present on every block (worst on grayscale stone) and was stable with camera
motion, i.e. locked to geometry rather than screen pixels. For a gray-in /
gray-out pipeline, any non-gray RGB coming back on a stone edge means the
palette lookup itself is returning the wrong entry at quad boundaries.

The single-`plr` layout works correctly only if the palette RAM presents an
asynchronous read (MLAB: combinational `palette[addr]`, registered into
`plr_src_rgb` at the next clock edge). If Quartus ignores the `ramstyle`
hint and demotes the array to M10K, the primitive has its own internal
address register, which inserts a hidden 1-cycle delay on the read. In that
case `plr_src_rgb` ends up holding `palette[palette_src_addr_from_prev_cycle]`,
so the first pixel of every new quad takes the previous quad's palette color.
That is exactly the "colored fringe at every block border, worst on stone"
artifact we were observing.

To make the pipeline correct regardless of how the palette is implemented,
the palette read is now split across two pipeline stages:

  * `pal_rd` (new) registers the palette source/fog addresses
    (`palette_src_addr`, `fog_color`) along with every other field fog0
    needs. The addresses are stable and flopped on one full cycle.
  * `plr` (existing, now consumes `pal_rd_*`) registers
    `palette[pal_rd_src_addr]` and `palette[pal_rd_fog_addr]`.

With the address flop in place, MLAB and M10K both produce the correct
palette entry at `plr_src_rgb` / `plr_fog_rgb` — MLAB reads combinationally
out of `pal_rd_*_addr` and gets flopped by `plr`, while M10K uses
`pal_rd_*_addr` as its own internal-register input and delivers data on
the cycle after `plr` would have captured it. Either way, every field
`fog0` consumes arrives on the same clock edge.

`DRAW_FLUSH_CYCLES` was bumped from 13 to 14 to account for the extra
pipeline stage, and `pal_rd_valid` is invalidated alongside the other
`*_valid` registers in ST_IDLE / ST_FETCH. `ST_CLEAR` still uses the
direct `clear_rgb565` combinational wire for the background; it does
not go through `pal_rd` / `plr`.

Cost: +1 cycle of end-to-end latency per pixel on top of the previous
change, a handful of additional flops, and no change to the software-
visible interface.

Verification (next reflash):
  1. Confirm the colored borders are gone.
  2. Sanity-check `output_files/*.fit.rpt` for the palette RAM type; with
     this fix we no longer care which one Quartus picks, but it's useful
     to know (grep for `palette`).
  3. Optional regression: temporarily force every face to light bank 0
     in `choose_face_light_flags` and verify the image stays clean.

Follow-up: Explicit Texture ROM (pipe2_tex_addr path)
-----------------------------------------------------
After the two-stage palette read was flashed, chromatic fringing was *still*
present on every block edge, and worse, an edge-flicker had appeared near
the right side of the screen (pink tint sometimes leaking in). User also
verified the palette was correctly inferred as MLAB cells from the fit
report, so the remaining skew was not on the palette side.

The next suspect was the texture atlas itself. `texture_mem` was previously
inferred from:

  (* ramstyle = "M10K" *) logic [7:0] texture_mem [0:TEXTURE_BYTES-1];
  ...
  always_ff @(posedge clk) tex_rd_data <= texture_mem[pipe2_tex_addr];

Quartus is free to implement this two different ways on M10K:
  (a) `tex_rd_data` is absorbed as the M10K's output register with
      `address_reg_a = CLOCK0`, `outdata_reg_a = UNREGISTERED`.
      Latency = 1 cycle. pipe2_tex_addr[T] -> tex_rd_data[T+1]. CORRECT.
  (b) The M10K keeps both its internal address register *and* its
      optional output register, with `tex_rd_data` layered on top as an
      extra flop. Latency = 2 cycles. pipe2_tex_addr[T] -> tex_rd_data[T+2].
      The first pixel of every new quad then samples whatever texel the
      previous quad's final `pipe2_tex_addr` happened to point at.

On silicon we had been hitting case (b). The visible effect was exactly the
chromatic aberration symptom: every block boundary showed a 1-pixel fringe
whose color depended on the immediately preceding quad's last texel. Stones
next to sky quads picked up light-blue fringes (their first-pixel address
landed inside the sky tile). Forcing every face to light bank 0 only
changed *which* palette entries the stale texels mapped to, not the
presence of the skew -- which matches the observed behavior (aberration
changed color but did not go away).

Fix: instantiate `texture_mem` as an explicit `altsyncram` ROM via a new
`voxel_texture_rom` module, pinning the configuration to:

  * `operation_mode = "ROM"`
  * `address_reg_a = CLOCK0` (implicit for port A on M10K)
  * `outdata_reg_a = "UNREGISTERED"`
  * `init_file = "voxel_gpu/assets/textures.mif"` (see the
    `altsyncram init_file` footnote below for why we moved off the
    Verilog `$readmemh` format)
  * `ram_block_type = "M10K"`

That gives a guaranteed 1-cycle latency: `pipe2_tex_addr` drives the
combinational address, the address is flopped once inside the M10K at the
clock edge, and the data appears combinationally on `rd_data` (= `tex_rd_data`)
on the following cycle. `tex_rd_data` is now a `wire`, not a reg, and the
`always_ff tex_rd_data <= texture_mem[...]` line has been removed along
with `$readmemh("textures.hex", texture_mem)` -- the altsyncram loads the
atlas directly from `voxel_gpu/assets/textures.mif` via `init_file`.

This change does *not* introduce a new pipeline stage, so
`DRAW_FLUSH_CYCLES` and the `pal_rd` / `plr` counts are unchanged. It only
removes Quartus's freedom to pick 2-cycle latency for the texture fetch.

Debug handle (software-side)
----------------------------
A `DEBUG_FLAT_COLOR` compile-time switch was added to `sw/renderer.c`. Build
with `-DDEBUG_FLAT_COLOR` (optionally `-DDEBUG_FLAT_COLOR_INDEX=<n>`, default
24) to force every emitted quad to render flat with `QUAD_FLAG_TEX` cleared
and `tex_or_color = <n>`. This bypasses the texture ROM + UV interpolation
+ light-bank path entirely, so:

  * If edges look clean in `DEBUG_FLAT_COLOR` mode, any remaining
    aberration is in the texture sampling path.
  * If edges still fringe in `DEBUG_FLAT_COLOR` mode, the problem is
    downstream (palette, fog, framebuffer RMW, or VGA scanout).

Useful for narrowing future regressions without rebuilding the RBF.

Verification (next reflash):
  1. Colored edges on stone/dirt/grass should be gone with the texture ROM
     fix. Primary success criterion.
  2. Optional: `grep palette output_files/*.fit.rpt` and
     `grep texture_rom output_files/*.fit.rpt` to confirm the memory
     types Quartus picked. (texture_rom will now show as an altsyncram
     M10K instance, not an inferred array.)
  3. Optional: rebuild game with `-DDEBUG_FLAT_COLOR` and verify the whole
     world renders as a uniform color with no fringing at block borders.

Footnote: `altsyncram` init_file format
---------------------------------------
Quartus's `altsyncram` `init_file` parameter only accepts an Altera
Memory Initialization File (`.mif`) or an **Intel-format** `.hex` file
(record-based with checksums). It does NOT accept Verilog
`$readmemh`-style files (plain bytes, one per line), which is what our
old `textures.hex` was.

We picked `.mif` as the **single source of truth** for the texture
atlas (no more sidecar files, no more drift risk):

  * `hw/voxel_gpu/scripts/generate_textures.py` emits only
    `hw/voxel_gpu/assets/textures.mif`
    (`WIDTH=8`, `DEPTH=16384`, address ordering `tile << 8 | (v << 4) | u`).
  * `voxel_texture_rom`'s `init_file` points at
    `voxel_gpu/assets/textures.mif`, and that MIF is added as a file in
    `voxel_gpu_hw.tcl`
    so Qsys copies it into `soc_system/synthesis/submodules/`.
  * The Python virtual hardware parses the same file at runtime via
    `virtualhw.raster.load_texture_mif`. The MIF parser understands
    `WIDTH`/`DEPTH`/`ADDRESS_RADIX`/`DATA_RADIX`, `addr : val;`
    entries, `[lo:hi] : val;` / `[lo..hi] : val;` range fills, and
    `--` comments. Unspecified addresses default to zero, matching
    Quartus's behaviour.

If you change the atlas generator, re-run
`python3 voxel_gpu/scripts/generate_textures.py` from `hw/`
and both synthesis and simulation pick up the new bytes automatically.
`$readmemh` is no longer used for the texture atlas anywhere in the
tree; the only remaining `$readmemh` is for `recip_lut.hex`, which
continues to use the plain-byte Verilog format because the reciprocal
LUT is still an inferred MLAB array, not an altsyncram instance.

April 2026: Chromatic Fringes on Far-Chunk Greedy Merges
--------------------------------------------------------

Summary
-------
After the SDRAM component landed on `main`, visible 1-pixel colored fringes
re-appeared on distant terrain. The pattern was distinctive: faint seams running
along block boundaries inside large flat regions (big grass plateaus, long
stone cliffs), only on chunks outside `NEAR_CHUNK_RADIUS`, and only on the
textured faces. Near-camera geometry was clean. This looked like chromatic
aberration but was actually a tiling seam.

Root Cause
----------
Two recent commits on `main` collided:

  * `1108cff` turned on greedy face merging for far chunks in
    `sw/world.c::rebuild_chunk_faces` so a run of identical blocks along the
    face's u/v axes ships as one `u_size * v_size` quad.

  * `83b47b5` added `QUAD_TEX_REPEAT_UV` (bit 6 of `tex_or_color`) plus the
    `texture_coord()` function in `hw/voxel_gpu/rtl/voxel_gpu.sv`. When the repeat flag is
    set, `texture_coord()` selects the texel with `value[35:32]`, i.e. it
    throws away the integer tile index and takes `u mod 16`.

When a merged quad was emitted with `QUAD_TEX_REPEAT_UV`, the rasterizer
interpolated the U axis from 0 to `16 * u_size`, wrapped via mod 16 every 16
texels, and sampled the same atlas tile over and over. The atlas tiles were
not authored to be seamless: texel column 15 on the grass tile does not equal
texel column 0. At every block boundary inside the merged run the sampler
jumped from column 15 straight back to column 0, producing the 1-pixel seam.
`apply_light_bank` amplified the effect because the two adjacent texels often
sat in different light banks.

A smaller secondary bug was that `texture_coord()` returns `4'd0` when
`value <= 0`, even in repeat mode, so negative interpolation rounding at the
top/left edge of a merged face clamped to texel 0 instead of wrapping to 15.

Fix
---
We stopped asking the hardware to wrap UVs and instead kept each textured
quad inside a single atlas tile. `sw/renderer.c::emit_merged_block_face` now
decomposes any `u_size > 1` or `v_size > 1` run into `u_size * v_size` unit
quads before emitting. The recursion ends immediately at `u_size == v_size ==
1`, reusing the existing emit path and its `is_face_visible` per-cell check.

`QUAD_TEX_REPEAT_UV` and `texture_coord()` remain in the hardware; they are
just never set by the current software. To A/B test the old fused path at
runtime without a rebuild, set `BLOCK_GAME_MERGE_FAR_QUADS=1`. The variable is
cached on first read so the branch picks one path for the whole session.

The software expansion cost is bounded by `MAX_QUADS_IN_FLIGHT`; when that
cap is hit mid-run the expander returns and the outer loop in
`renderer_draw_world` bails out on the same check it already used.

Contingency
-----------
If the fringe ever reappears after this fix, treat it as a different bug and
retrace using `DEBUG_FLAT_COLOR` (see earlier section in this file). A flat
color with the same pattern means the issue is in the rasterizer or a later
pipeline stage, not in the sampler. With `DEBUG_FLAT_COLOR` off, the presence
or absence of the fringe while toggling `BLOCK_GAME_MERGE_FAR_QUADS` isolates
whether merging is back in the critical path.

Follow-up: Keep Merging, Fix Repeat Coordinates
-----------------------------------------------

Expanding every far merged face into unit quads reduced the use of
`QUAD_TEX_REPEAT_UV`, but it also removed most of the performance reason for
far-chunk greedy meshing. On hardware this can show up as uneven frame pacing
and delayed keyboard response because the descriptor stream grows back toward
the unmerged worst case.

The repeat path itself had a real RTL bug: `texture_coord()` checked
`value <= 0` before `repeat_uv`, so a slightly negative fixed-point coordinate
at the top/left edge of a repeated quad clamped to texel 0. Correct modulo
repeat should take the low 4 integer bits for both positive and negative
values; in two's-complement that is simply `value[35:32]`.

May 8 atlas-capacity follow-up: the 128-tile atlas needs seven tile-id bits,
so `QUAD_TEX_REPEAT_UV` moved from `tex_or_color[6]` to `tex_or_color[7]`.
The hardware sampler now uses `tex_or_color[6:0]` for the tile address and
bit 7 only for coordinate wrapping. This preserves the doubled atlas while
keeping repeated far-quad UVs unambiguous.

The 128-tile atlas made the current fit land just over the device limit
(400/397 M10Ks). Spilling small side FIFOs into MLAB fixed M10K pressure but
then missed LAB placement (3242/3207 LABs), so the safer resource trade is to
keep MLAB pressure low and trim the GPU command FIFO from 2048 to 1024 words.
That saves the needed M10Ks without changing the raster/cache datapath, at the
cost of some descriptor-upload buffering headroom.

The hardware now applies the repeat case first. The renderer once again uses
merged far quads by default, and `BLOCK_GAME_MERGE_FAR_QUADS=0` remains as a
runtime fallback to force unit-quad expansion for A/B testing.

Future Architecture Idea: True 640x480 Resolution via Direct-to-SDRAM Z-Cache
-----------------------------------------------------------------------------

Currently, the renderer is limited to a 320x240 internal resolution upscaled 2x2 because a single 16-bit RGB565 backbuffer and 16-bit Z-buffer completely exhaust the FPGA's ~512 KB of internal M10K block RAM. Expanding to a true 640x480 resolution would require ~1.2 MB, which physically will not fit on the Cyclone V.

To achieve true 640x480 in the future without moving to a Tile-Based Rendering (TBR) approach, we could restructure the rasterizer to write directly to the massive external SDRAM. 

However, because the Z-buffer requires a Read-Modify-Write cycle for every single pixel, the latency of SDRAM would bottleneck the 1-pixel-per-cycle pipeline. The solution is to eliminate the full-frame internal BRAM buffers entirely and replace them with a microscopic BRAM Z-Cache (e.g., an 8x8 pixel block, taking only 256 bytes total).

The rasterizer would operate on this tiny cache at full speed. When it moves out of the 8x8 bounds (a cache miss), the hardware would stall, flush the dirty cache block out to SDRAM via burst writes, and burst-read the new block from SDRAM. This provides the massive resolution limits of SDRAM while retaining the high-speed math capability of the FPGA's internal memory.

Cache Sizing & The "Scanline Cache" Optimization
------------------------------------------------
By removing the full 320x240 framebuffers from BRAM, we free up approximately 400 KB of internal memory on the Cyclone V. Because each pixel takes 4 bytes (2 for RGB565, 2 for Z-depth), we could theoretically cache up to ~100,000 pixels at once—nearly 1/3rd of the entire 640x480 screen.

However, SDRAM incurs massive latency penalties for 2D memory access patterns (like an 8x8 square) because it destroys burst efficiency. To optimize SDRAM bursts, the cache shape should be 1-dimensional. 

A highly optimized architecture would use a **Scanline Cache**. Using the 400 KB of free BRAM, we could cache a horizontal block of 640x32 pixels (roughly 81 KB). 
- SDRAM can burst-read/write full horizontal lines with maximum efficiency.
- The rasterizer stays within the 32-row vertical bounds for a long time, drawing quads at the full 50 MHz rate.
- A cache miss only occurs when the rasterizer crosses the 32-row boundary, meaning the massive SDRAM latency penalty is amortized over thousands of rapid cache hits, preserving the framerate.

April 2026: First-Pass Full-Resolution Migration Plan
-----------------------------------------------------

Goal
----
Unlock true 640x480 rendering without requiring a software-side banding or
quad-sorting pass up front. Software should be allowed to emit ordinary quads in
the same order it does today. Hardware owns correctness; later software work can
make the access pattern friendlier.

The shared software geometry has been moved to `sw/voxel_gpu.h`:

  * `VOXEL_RENDER_WIDTH = 640`
  * `VOXEL_RENDER_HEIGHT = 480`
  * `VOXEL_RENDER_STRIDE = 1280` bytes
  * `VOXEL_BAND_CACHE_HEIGHT = 64`
  * `VOXEL_BAND_COUNT = 8`

`sw/renderer.h` now derives `SCREEN_WIDTH` and `SCREEN_HEIGHT` from those
constants, and the virtual hardware server's default framebuffer is 640x480.
That is the software-facing target. The hardware still needs the coherent RTL
cache migration below before this is safe on FPGA.

Why not directly use SDRAM as `fb_back_ram` / `z_ram`?
-----------------------------------------------------
The current raster pipeline depends on one-cycle local reads for both depth and
destination color:

  * `z_rd_data` becomes `recip1_z_ref`
  * `fb_back_rd_data` becomes `recip1_dst_rgb565`

Every candidate pixel may perform:

  1. read z
  2. read destination RGB565 for alpha/fog blending
  3. compare z
  4. write color
  5. write z

The board SDRAM controller is FIFO/burst oriented, not a low-latency random
read/write port. Treating it like BRAM would collapse the one-pixel-per-cycle
pipeline. The first-pass design must therefore keep the hot read-modify-write
path in BRAM and use SDRAM only at cache boundaries.

Hardware contract
-----------------
The first full-resolution RTL pass should implement a hardware-managed vertical
band cache:

  * Cache shape: 640x64 pixels.
  * Cache contents: RGB565 color cache + 16-bit z cache.
  * Hot-path storage cost: 640 * 64 * 4 bytes = 160 KiB.
  * The cache is the only place the raster pipeline reads/writes per-pixel
    color and z.
  * SDRAM is the backing store for the full inactive render frame and the full
    z frame.

For each rasterized pixel:

  * If `pixel_y` is inside the resident band, issue the pixel into the existing
    pipeline using the band-local BRAM address.
  * If `pixel_y` misses the resident band, stop issuing new pixels, drain the
    existing pipeline, flush the dirty resident band to SDRAM, load or initialize
    the target band, then retry the same pixel.

Correctness requirement: any descriptor that was legal in the old 320x240
pipeline must render correctly in 640x480 space. Cache misses may stall, but
they must never drop pixels or require software to split quads.

Frame/band metadata
-------------------
Keep tiny per-frame metadata in RTL:

  * `render_target_sel`: inactive color frame selected at frame begin.
  * `band_valid_this_frame[7:0]`: whether a band has already been initialized or
    flushed during this frame.
  * `cache_valid`: whether the BRAM cache currently contains a resident band.
  * `cache_dirty`: whether the resident band must be flushed before eviction.
  * `cache_band_index`: resident vertical band, `pixel_y / 60`.

At `CLEAR_FRAME` / frame begin:

  * Select the inactive SDRAM color frame as the render target.
  * Clear `band_valid_this_frame`.
  * Invalidate the BRAM cache.
  * Do not clear the whole SDRAM frame; bands are initialized lazily.

On first touch of a band this frame:

  * Initialize BRAM color to the clear/background color.
  * Initialize BRAM z to `16'hffff`.
  * Mark the band resident but not yet backed by meaningful SDRAM contents.

On revisiting a band already touched this frame:

  * Burst-read the band color from the inactive render frame.
  * Burst-read the band z from the z backing region.
  * Resume the stalled pixel after both caches are filled.

On evicting a dirty band:

  * Burst-write the band color to the inactive render frame.
  * Burst-write the band z to the z backing region.
  * Mark `band_valid_this_frame[cache_band_index] = 1`.

The final band must be flushed before honoring `FLIP`; then the visible color
frame can swap on vsync. The old full-frame `ST_COPY` becomes unnecessary once
all rendering writes through the band cache.

SDRAM layout
------------
Default byte layout for the first pass:

  * front color frame: byte base `0`
  * back color frame: byte base `640 * 480 * 2 = 614400`
  * z backing frame: byte base `2 * 614400 = 1228800`

Color scanout only reads whichever color frame is visible. Rendering writes the
inactive color frame and the z backing frame.

With the current 60-line bands, band 7 covers rows 420..479. Flush/load logic
must keep the SDRAM word offset and pixel count in the same 60-line geometry as
the descriptor binning; mixing a 64-line stride with 60-line bins creates a
vertical offset that grows by four rows per band.

SDRAM read arbitration
----------------------
This is the main RTL hazard.

The existing SDRAM read FIFO is already used by VGA scanout line-fill. Cache
loads also need SDRAM reads. The first functional implementation may give cache
loads priority during active rendering, which can temporarily starve scanout of
the previous frame while the next frame is being rasterized. That is acceptable
for bring-up if documented and visible output is stable after `FLIP`, but the
arbitration must be explicit.

Preferred first-pass policy:

  1. Scanout owns reads when the engine is idle or only flushing writes.
  2. Cache-load states own reads while the rasterizer is stalled on a miss.
  3. Cache loads run in 64-word bursts aligned to scanlines.
  4. After the frame is complete and `FLIP` is accepted, scanout regains the read
     port exclusively.

Later optimization can interleave cache-load bursts only when scanout has enough
prefetched line data, but that is not required for first correctness.

RTL implementation steps
------------------------
Implement as one coherent RTL change, not as a half-swapped framebuffer:

  1. Change `FB_WIDTH`, `FB_HEIGHT`, `LINE_WORDS`, stride defaults, and VGA
     scanout indexing to true 640x480.
  2. Replace full-frame `fb_back_ram` and `z_ram` with 640x64 band RAMs.
  3. Convert draw addresses from full-frame addresses to band-local cache
     addresses: `(pixel_y - cache_band_y0) * 640 + pixel_x`.
  4. Add miss detection before issuing `pipe0_valid`.
  5. On miss, suppress new `pipe0_valid`, drain the current pipeline, and enter
     cache flush/load/init states.
  6. Add color-band flush, z-band flush, color-band load, z-band load, and
     init-band states.
  7. Replace `ST_COPY` with final dirty-band flush before vsync swap.
  8. Preserve the existing descriptor ABI; software does not need to sort or
     split quads for correctness.

Software follow-ups after hardware is correct
---------------------------------------------
Once full-resolution hardware is running, software can improve frame pacing
without changing correctness:

  * Treat sky/background as band initialization instead of ordinary fullscreen
    quads.
  * Sort opaque descriptors by vertical band.
  * Split very tall screen-space quads at band boundaries.
  * Keep translucent/UI passes ordered more carefully.

Those are optimizations only. The hardware-managed cache remains the fallback
that makes arbitrary descriptor order valid.

RTL status
----------
The first-pass RTL migration has now been implemented in
`hw/voxel_gpu/rtl/voxel_gpu.sv`.

What changed:

  * The raster/display geometry is now 640x480.
  * VGA scanout indexes true 640 columns and 480 rows instead of 2x upscaling a
    320x240 internal surface.
  * The full-frame BRAM color and z memories were replaced with 640x64 band
    memories.
  * Draw-pipeline addresses are now band-local cache addresses.
  * A draw pixel only enters `pipe0` if its vertical band is resident.
  * A miss drains the pipeline, flushes the dirty resident band if needed, then
    either initializes or reloads the target band before retrying the same
    pixel.
  * `FLIP` now waits for the final dirty resident band to flush, then swaps the
    visible SDRAM color frame on vsync.

Verification performed locally:

  * `verilator --lint-only` on the GPU RTL and SDRAM support files passes when
    the expected Quartus vendor primitives (`altsyncram`, `dcfifo`,
    `altera_pll`) and legacy SDRAM-controller width warnings are suppressed.
  * Software renderer test binaries build on macOS.
  * Virtual hardware tests pass at 640x480.

Bring-up caveats:

  * This has not yet been through Quartus fit/timing.
  * The z backing base is currently fixed at byte address `1228800`
    (`2 * 640 * 480 * 2`) in RTL rather than exposed as its own CSR.
  * Cache-load reads temporarily take ownership of the SDRAM read side over VGA
    scanout. That is acceptable for the first correctness pass, but visible
    scanout starvation while rendering is possible until we add smarter read
    arbitration or software-side cache-friendly ordering.

Follow-up: Stabilize Scanout During Cache Maintenance
-----------------------------------------------------
The first hardware try after the band-cache migration produced a flashing mess.
That matched the main risk above: cache maintenance and VGA scanout were sharing
one SDRAM controller, but cache work could keep feeding the SDRAM FIFOs even
when scanout was about to run out of prefetched line data.

Two fixes were made in `voxel_gpu.sv`:

  * Added `scanout_slack`, which is true only when no frame is displayed yet,
    VGA is blanking, or scanout already has the next visible line buffered.
    Cache flush writes and cache load reads now advance only during those slack
    windows. This favors a stable displayed frame over render throughput.
  * Fixed cache-load setup so `RD_LOAD` is only pulsed in states where
    `sdram_rd_length_cfg` is non-zero. Previously the color/z cache-load start
    pulses were issued from setup states while the length mux still returned
    zero, which could make revisited-band reloads unpredictable.

Expected effect: flashing/black-line instability should be reduced to stalls or
lower frame rate. If the image is stable but slow, the remaining work is
performance policy rather than basic SDRAM ownership correctness.

Additional quality fix: exclusive SDRAM read ownership
------------------------------------------------------
The first stabilization pass still allowed cache loads to start during a slack
window even if scanout had an outstanding line-fill burst or leftover words in
the shared SDRAM read FIFO. That can mix scanout words into cache loads (or vice
versa), which shows up as flashing, sparkle, or wrong bands rather than a simple
slow frame.

The RTL now has `cache_read_start_ok`, which requires:

  * scanout slack,
  * no active scanout fill,
  * no armed scanout read,
  * no delayed scanout pop, and
  * an empty SDRAM read FIFO.

Cache color/z loads only pulse `RD_LOAD` under that condition. This is
intentionally conservative: scanout owns the read side unless it is fully idle.
The expected tradeoff is lower render throughput, but the visible frame should
be much less likely to tear or flash due to mixed FIFO ownership.

Follow-up: Avoid cache-read deadlock and make CLR recover the engine
--------------------------------------------------------------------
The stricter read-start gate fixed ownership but exposed a liveness problem:
once a cache read burst has already started, continuing to gate every FIFO pop
on scanout slack can strand cache words in the read FIFO. The engine then stays
busy in a cache-load state, which makes the next userspace `CLEAR_FRAME` ioctl
time out because the old RTL only consumed `clear_pending` from `ST_IDLE`.

Two liveness fixes were added:

  * Cache reads still require exclusive ownership before pulsing `RD_LOAD`, but
    once a cache burst is in flight the cache drains the read FIFO continuously
    until the band load completes.
  * A CONTROL write with CLR set is now treated as an engine abort/restart. It
    invalidates cache state, drops pending flip/cache work, clears the descriptor
    FIFO and pipeline valid bits, and returns the state machine to `ST_IDLE`
    immediately instead of waiting behind the previous frame.

This is deliberately biased toward recovery. If a frame hits a bad cache
maintenance corner, the next frame begin should unwedge the hardware rather than
leaving `/dev/voxel_gpu` stuck in timeout loops.

April 2026: Band-Pass SDRAM Renderer Implementation
---------------------------------------------------
The hardware-managed random band cache was the wrong first full-resolution
shape. It could preserve descriptor order, but it forced the raster engine,
cache refill path, cache flush path, and VGA scanout to fight over one
FIFO-style SDRAM read/write path. On hardware this showed up as flashing, then
black frames with `CLEAR_FRAME` / descriptor writes timing out.

The replacement first pass is explicit band rendering:

  * Full render target remains 640x480 RGB565 in SDRAM.
  * The on-chip working set is one 640x96 color band plus one 640x96 z band.
  * There are exactly five bands: rows 0-95, 96-191, 192-287, 288-383, and
    384-479.
  * `CLEAR_FRAME` selects the inactive SDRAM color frame and resets band/FIFO
    state. It does not clear a full framebuffer.
  * `BEGIN_BAND(index)` clears the resident 96-line color/z band on chip.
  * Userspace writes only the descriptors that overlap that band.
  * `END_BAND` drains the FIFO/raster pipe and flushes the color band to the
    inactive SDRAM frame at `frame_base + band_index * 640 * 96` words.
  * `FLIP` only waits for the frame to be complete and swaps visible SDRAM
    frames on vsync.

Software binning is intentionally isolated in `sw/gpu_transport.c`. The
renderer still builds one packed descriptor buffer for the whole frame. The
transport parses that buffer, duplicates each descriptor into every overlapping
band bin, and wraps each hardware bin with `BEGIN_BAND` / `END_BAND`. The
virtual socket backend keeps receiving the original whole-frame stream so the
Python rasterizer remains a simple reference model.

This is conservative but deterministic. It removes all SDRAM color reloads and
all SDRAM z traffic from the first hardware pass. Later, once the full-res path
is stable, the transport can get smarter without changing `renderer.c`: sky can
be treated as a per-band clear/background, large full-screen quads can be
special-cased, and repeated descriptor duplication can be replaced with tighter
software bins.

April 2026: Column-0 Red Speckle Bug (SDRAM Read FIFO Residual)
---------------------------------------------------------------

Symptom
-------
After the SDRAM full-resolution upgrade (640x480 with 5 x 96-line bands),
intermittent red specks appeared at the absolute left edge of the screen
(column 0). Specks were sparse, time-varying, and only ever in the leftmost
column. Red is `palette[0x06]` aka the block-fault color, which is the
worst-visible stale value when something goes wrong upstream.

Wrong Hypotheses (ruled out)
----------------------------
We first chased software-side stale-state hypotheses:

  * Per-quad pipeline staleness on the first descriptor of each band - tested
    by prepending a 1x1 hidden primer quad per band in `sw/gpu_transport.c`.
    No effect, ruling this out.
  * `ST_CACHE_INIT` first-cycle stale-address race - claimed by an investigation
    pass but verified incorrect: `cache_maint_addr <= 0` and `state <= ST_CACHE_INIT`
    are non-blocking assignments in the same `always_ff` and update on the same
    edge.

Root Cause
----------
The scanout linebuffer fill path was popping the SDRAM read FIFO before the
just-programmed burst's data had actually arrived. In `hw/voxel_gpu/rtl/voxel_gpu.sv`
the pop wire was originally:

    wire scan_rd_pop = scan_fill_active && !sdram_rd_empty &&
                       (scan_fill_words_complete < LINE_WORDS_10) &&
                       !scan_fill_chunk_done;

`scan_fill_armed=1` means `sdram_rd_load_pulse` was just asserted to program
a new 64-word burst, but the SDRAM controller has not yet started delivering
data for it. During that window any `!sdram_rd_empty` is residual from the
prior burst, not new line data. The pop wire did not gate on `!scan_fill_armed`,
so on rare timings the very first word into `scan_linebuf[0]` (column 0 of the
line) came from the prior burst's tail. With showahead=OFF on the SDRAM read
dcfifo, the 1-cycle rdreq->q latency made this race visible only sometimes.

Fix
---
Gate the pop wire on `!scan_fill_armed`:

    wire scan_rd_pop = scan_fill_active && !scan_fill_armed && !sdram_rd_empty &&
                       (scan_fill_words_complete < LINE_WORDS_10) &&
                       !scan_fill_chunk_done;

`scan_fill_armed` clears one cycle after `!sdram_rd_load_pulse && !sdram_rd_empty`
(see lines ~1995 in `voxel_gpu.sv`), i.e. once the new burst's data has actually
landed in the FIFO. Pops resume the cycle after that. Worst case we add one
cycle of latency at burst-start; with `LINE_WORDS_10=10` 64-word bursts per
scanline and ample slack before scanout consumes the line, this is harmless.

Why column 0 specifically: the contaminated word was always the first word of
the first burst of a scanline, i.e. `scan_linebuf[0]`, which the scanout shifts
out as the leftmost pixel. Subsequent words came from the correct burst.

Confirmed fixed on hardware after reflash.


April 30 2026: FPS Bottleneck Diagnosis
---------------------------------------

Problem
-------
After moving from 320×240 on-chip-only framebuffer to 640×480 SDRAM-backed
banding, FPS dropped from ~60 to 6–15 FPS. The perf counters show:

    update = 19–69 ms   (chunk meshing / lighting)
    draw   = 12–20 ms   (CPU descriptor emission, ~1800 quads)
    end    = 47–60 ms   (hardware rasterization + band flush + vsync)

Total frame time: 80–150 ms. Need ≤33 ms for 30 FPS.

Diagnosis: Where the Time Goes
------------------------------
The `end` phase calls `renderer_end_frame()`, which loops over 5 bands:

    for each band 0..4:
        BEGIN_BAND ioctl    → kernel polls BSY until ST_CACHE_INIT finishes
        write() descriptors → kernel pushes into 1024-word FIFO, blocks when full
        END_BAND ioctl      → kernel polls BSY until rasterizer + cache flush done
    FLIP ioctl              → kernel polls until next vsync

The CPU is idle during `end`. It's blocked in kernel-space `udelay()` polls
waiting for the hardware. The hardware bottleneck is the rasterizer:

  * 1 pixel evaluated per FPGA clock at 50 MHz.
  * ~1800 world quads × ~1400 pixels average bounding box = ~2.5M pixel evals.
  * 2.5M / 50 MHz = **50 ms** of pure rasterization. Matches observed `end`.

The SDRAM cache flush (ST_CACHE_FLUSH_COLOR) adds additional time but is gated
by `scanout_slack` to avoid bus contention with VGA scanout reads. Each flush
drains 61,440 words through the SDRAM write FIFO, interleaved with scanout
read bursts.

What We Tried (and Learned)
---------------------------
1. **Removed `scanout_slack` gate from cache flush** — Attempted to let SDRAM
   writes proceed during active scanout. Result: caused left-side pixel
   corruption (SDRAM read/write contention during VGA scanout). REVERTED.

2. **Flat buffer transport** (iovec→single write) — Reduced per-band syscall
   overhead. Helped CPU-side transport cost but didn't touch the hardware
   rasterization time, which dominates.

3. **Quad budget cap** (WORLD_QUAD_BUDGET=800) — Capping world quads would
   bring `end` under 20 ms but degrades visual quality unacceptably. REVERTED.

What Actually Helps
-------------------
1. **Early scanline exit** (RTL, applied) — For convex quads, once the edge
   function transitions inside→outside while scanning a row left-to-right,
   all remaining pixels on that row are outside. Skip to next row. Saves
   ~30–40% of pixel evaluations for typical perspective-projected quads.
   Zero resource cost.

2. **Duplicate rebuild fix** (game.c, applied) — The lighting + mesh rebuild
   block was copy-pasted twice in the game loop. Removing the duplicate cuts
   `update` spikes roughly in half.

3. **Faster kernel polling** (voxel_gpu.c, applied) — Reduced udelay from
   10 μs to 1 μs. CPU reacts faster when FIFO drains or BSY clears.

4. **Larger bounce buffer** (voxel_gpu.c, applied) — 2 KB → 8 KB. Fewer
   copy_from_user transitions per band.

5. **Ping-pong band cache** (RTL, planned) — See below.

Ping-Pong Band Cache Plan
--------------------------
### The Problem

Per-band processing is fully serial:

    Band 0: INIT(1.2ms) → DRAW(X ms) → FLUSH(Y ms)
    Band 1: INIT(1.2ms) → DRAW(X ms) → FLUSH(Y ms)
    ...

The INIT and FLUSH phases are pure overhead. INIT writes 61,440 pixels of
clear values to fb_back_ram + z_ram (one pixel/cycle = 1.2 ms). FLUSH reads
61,440 words from fb_back_ram and streams them to SDRAM (throttled by
scanout_slack, ~2–12 ms depending on VGA timing).

### The Fix

Duplicate the band cache (fb_back_ram + z_ram). While the rasterizer draws
into cache A, cache B can be flushing to SDRAM. When band N finishes drawing,
swap: start drawing band N+1 into cache B, start flushing cache A.

    Cache A: [INIT+DRAW band 0] ──────────── [INIT+DRAW band 2] ────
    Cache B:            [FLUSH 0 + INIT+DRAW band 1] ──── [FLUSH 1 + DRAW 3]

This overlaps FLUSH with DRAW, hiding the flush latency. 5 bands × ~2–12 ms
flush = 10–60 ms saved.

### Resource Cost

Current band cache:
  * fb_back_ram: 61,440 × 16-bit = ~120 KB (M10K)
  * z_ram:       61,440 × 16-bit = ~120 KB (M10K)
  * Total:       ~240 KB (47% of Cyclone V's 512 KB M10K)

Ping-pong doubles this to ~480 KB (94% of M10K). Tight but feasible. The
texture ROM (16 KB), FIFO (8 KB), palette (<1 KB), and scanline buffers (~3 KB)
fit in the remaining ~30 KB.

### Port Mapping

Each voxel_sdp_ram has one read port and one write port. With ping-pong:

    Active cache (rasterizer):
      write port → rasterizer pixel writes (fb_wr_addr/data)
      read port  → alpha-blend readback (pipe0_addr) + Z-test read (pipe0_addr)

    Inactive cache (flush):
      read port  → cache_maint_addr for SDRAM flush
      write port → cache INIT (clear values)

No port conflicts. The key insight: the flush controller only needs the read
port of the inactive cache, and INIT only needs the write port of the inactive
cache. Both are free while the rasterizer owns the active cache.

### Implementation Steps

1. Instantiate fb_back_ram_A, fb_back_ram_B, z_ram_A, z_ram_B
2. Add `draw_cache_sel` register (0=A, 1=B), toggled at band boundaries
3. Mux the rasterizer's read/write ports to the active cache
4. Add a parallel flush FSM (or extend the main FSM) that reads from the
   inactive cache and pushes to the SDRAM write FIFO
5. Overlap: when END_BAND fires, start the flush on the inactive cache
   AND immediately allow BEGIN_BAND to start INIT+DRAW on the active cache
6. The FLIP ioctl must wait for the last outstanding flush to complete before
   swapping SDRAM display pointers

RTL/Software Blank-Screen Follow-Up Pass (2026-05-01)
-----------------------------------------------------

### What was checked

After the renderer reported live HW submission, full-screen descriptor bounds,
and successful per-frame flips while the monitor stayed blank, the likely fault
surface moved downstream from game/descriptor generation into the SDRAM
copy/scanout path.

### Changes applied

1. **SDRAM read arbitration hardening** — Prevent scanout line fills and cache
   color/Z reloads from arming the shared SDRAM read controller in the same
   cycle. Cache reads now wait when a vsync scan fill or next-line scan fill is
   about to start.

2. **Read-pop completion fix** — Removed self-referential completion checks
   where `cache_rd_pop` depended on a value that also included `cache_rd_pop`,
   and similarly simplified `scan_rd_pop`. This avoids combinational feedback
   around the SDRAM read FIFO pop path.

3. **Scan/cache read isolation** — `scan_rd_pop` is gated off during cache load
   states, so cache reloads cannot accidentally be consumed by the scanline
   buffer path.

4. **Lint cleanup in `voxel_gpu.sv`** — Removed the dead
   `cache_rd_load_state`, made status packing a full 32 bits by exposing
   `flush_active`, changed 1-bit write-enable zeroes to `1'b0`, and made local
   comparisons/adds width-explicit. This does not change intended behavior, but
   makes future warnings more meaningful.

### Remaining potential issues

1. **Empty bands do not force an SDRAM clear** — A BEGIN/END pair with no quads
   initializes the on-chip band cache but does not mark it dirty, so that band
   may not be flushed to SDRAM. Full-screen sky/world geometry usually masks
   this, but sparse scenes can leave stale pixels. Fixing it is possible by
   marking initialized empty bands dirty, but that adds SDRAM traffic.

2. **Historical 64-line band stride hazard** — The 60-line fit relies on every
   band helper using the same geometry. If `band_word_offset()` or `y_to_band()`
   regresses to 64-line math, each lower band is placed four more rows away from
   where software binned it.

3. **Verilator cannot fully lint vendor IP here** — Local lint reaches useful
   warnings, but stops on missing Intel primitives/wrappers (`dcfifo`,
   `altsyncram`, PLL generated module). Remaining width warnings are in the
   SDRAM vendor/test controller, not the top-level voxel GPU logic.

DSP-Aware RTL Speedup Ideas (Future Work)
-----------------------------------------

Device context: the current board target is `5CSEMA5F31C6`. Treat Quartus as
the source of truth, but this Cyclone V class has enough variable-precision DSP
blocks that the renderer is unlikely to be fundamentally DSP-limited today.
The exact current usage should be checked after a full fit with:

    quartus_sh --flow compile soc_system
    rg -n "DSP|embedded multiplier|Resource" output_files/*.fit.rpt output_files/*.map.rpt

The current RTL uses a one-pixel-per-cycle raster pipe with many multiplies in
the setup/evaluate path:

  * edge functions: `edge_A/B * x/y`
  * z plane: `dz_dx/dz_dy * dx/dy`
  * perspective planes: `uw/vw/iw dx/dy`
  * UV reconstruction: `u_over_w * w`, `v_over_w * w`
  * fog radial math

That gives several plausible DSP-side speedups.

### 1. Incremental Edge/Z/UV Stepping

Right now each pixel recomputes plane values from `x/y` with multiplies. For a
quad, compute row-start values once, then advance each pixel with additions:

    edge(x + 1, y) = edge(x, y) + A
    z(x + 1, y)    = z(x, y)    + dz_dx
    uw(x + 1, y)   = uw(x, y)   + uw_dx

At end of row, advance row-start values by the `*_dy` terms.

Expected benefit: large ALM/DSP timing reduction and simpler critical path.
It may not consume more DSPs; it may free them. This is probably the first
speedup to do before widening the pixel pipe, because it makes every later
parallel lane cheaper.

Risk: must preserve the current exact edge inclusion behavior. Add a small RTL
or verilator-style reference test for shared-edge quads before changing this.

### 2. Two-Pixel Raster Pipe

After incremental stepping, evaluate two adjacent pixels per cycle:

    lane0 uses current values
    lane1 uses current + dx_step
    next cycle advances by 2 * dx_step

This needs duplicate compare/Z/texture/palette/fog/commit lanes, or at least a
split where two coverage/Z candidates feed a narrower commit backend. It also
needs two writes per cycle to the color and Z band caches. The current
`voxel_sdp_ram` instances expose one write port, so a true two-pixel commit
probably requires banking the band cache by even/odd x:

    color_even, color_odd, z_even, z_odd

Expected benefit: up to 2x raster fill for large quads, if memory ports and
texture/palette lookup keep up.

Cost: roughly doubles color/Z cache RAM instances or complicates write
arbitration. Given M10K pressure from ping-pong 640x64 bands, this is only
attractive if Quartus confirms comfortable M10K headroom.

### 3. Parallel Setup for Next Quad

Keep the existing one-pixel draw pipe, but add a setup pipe that precomputes
the next descriptor's edge constants, bbox clamps, row-start values, and
perspective increments while the current quad is still draining.

Expected benefit: hides descriptor/setup bubbles, especially for many small
quads like block faces and UI glyph rectangles.

DSP use: modest. This mainly uses extra registers and a few duplicate setup
multipliers/adders. It avoids the RAM-port explosion of a multi-pixel commit
pipe.

Risk: descriptor FIFO/control complexity. Keep the current descriptor ABI, but
add a small two-entry decoded descriptor queue.

### 4. Dedicated Fog/Blend DSP Pipeline

Fog currently adds several multiplies after texture/palette work. If fog or
alpha blending becomes part of the timing limiter, move it into a deeper,
dedicated pipeline with explicit DSP inference and registers between:

    center offset square -> radial scale -> depth scale -> blend

Expected benefit: higher Fmax and less pressure on the main draw pipe. It does
not reduce cycle count unless paired with wider pixel lanes, but it can make
more aggressive parallelism fit timing.

Risk: pipeline alignment bugs. Every added stage must carry `valid/pass/addr/z`
and source/destination color metadata exactly.

### 5. Tile/Span Fill Fast Path

Many screen/UI/sky quads and block faces cover contiguous spans. Add a fast
path that recognizes flat-color, no-texture, no-z-test spans and writes a run
of pixels without going through perspective texture and fog arithmetic. A
variant could issue two or four pixels per cycle into banked color RAM.

Expected benefit: big for sky, chat text shadows, hotbar rectangles, and simple
solid geometry. This saves DSP cycles for the textured path rather than using
more of them.

Risk: must not reorder translucent/UI quads relative to world quads. Treat it
as an internal execution optimization for descriptors that are already next in
stream order.

### Practical Order

Recommended sequence:

  1. Run Quartus fit and record actual DSP/M10K/Fmax.
  2. Convert edge/z/uv plane evaluation to incremental stepping.
  3. Add next-quad setup predecode if small-quad overhead is visible.
  4. Only then consider a 2-pixel pipe, because it likely needs band-cache
     banking and may trade DSP headroom for scarce RAM ports.

The main warning: spare DSPs alone do not guarantee a faster renderer. The
current architecture is also constrained by on-chip RAM ports, SDRAM flush
bandwidth, and VGA scanout arbitration. Use DSPs aggressively where they remove
the per-pixel critical path, but avoid a wider commit pipe until the memory-port
story is clean.

May 2026: Two-Pixel-Per-Cycle Pipeline Project
----------------------------------------------

Goal: lift FPS from ~15 to 30+ by widening the rasterizer to commit two
adjacent pixels per cycle. With logging on, the `end` phase (raster + flush +
vsync) takes ~56 ms; the dominant component is per-pixel evaluation at 1
px/cycle (~2.5M pixel cycles at 50 MHz ≈ 50 ms). Doubling raster throughput
should cut the visible portion of `end` roughly in half.

### Step 1: SDRAM PLL setup violation (diagnosis only, no SDC change)

Quartus reports `Slow 1100mV 85C` setup slack of **-2.734 ns** with
TNS -82.4 ns on the clock
`soc_system0|voxel_gpu_0|sdram_ctrl|sdram_pll0_inst|...|general[0].gpll~PLL_OUTPUT_COUNTER|divclk`.
That clock is `outclk_0` of the PLL embedded inside the Terasic-style
`Sdram_Control` (in `hw/sdram_local_test/`), driving `sdram_ctrl_clk` at 100
MHz. The PLL's restricted Fmax for that output is 121.05 MHz, so the violation
is not a Fmax-vs-target mismatch — it comes from cross-domain paths.

Root cause: `Sdram_Control` instantiates `Sdram_WR_FIFO` and `Sdram_RD_FIFO`
which are real Altera `dcfifo` async crossings (lpm_numwords=512, M10K-backed,
rdsync/wrsync delaypipe=4) between `clk` (50 MHz, `clock_50_1`) and the
internal `CLK` (100 MHz `sdram_ctrl_clk`). The project SDC
(`hw/soc_system.sdc`) is minimal — only base `clock_50_*`/`clock_27_1`,
`derive_pll_clocks -create_base_clocks`, and `derive_clock_uncertainty`. There
is no `set_false_path`, `set_clock_groups -asynchronous`, or per-FIFO
constraint, so Quartus times the gray-code pointer synchronizer paths through
the FIFO as synchronous 50→100 MHz. The 4-stage synchronizer plus pointer
comparators won't meet a 5 ns launch-to-latch window, which matches the
~-2.7 ns slack and large negative TNS.

The same FIFO usage pattern in any standard Altera reference design is
covered by an IP-supplied `_constraints.sdc`/`.qip`-generated constraint
file. This codebase imported `Sdram_WR_FIFO.v` / `Sdram_RD_FIFO.v` as
hand-pasted dcfifo templates without their accompanying SDC, so the
async pointer paths are timed.

Fix options (not applied this pass):

  1. Add to `hw/soc_system.sdc`:
     `set_false_path -from [get_clocks {clock_50_1}] -to [get_clocks {*voxel_gpu_0|sdram_ctrl|sdram_pll0_inst*outclk_0*}]`
     plus the reverse direction. Risk: also cuts WR_LOAD / RD_LOAD / WR_ADDR
     handshake paths from `clk` into `CLK`, which are real ad-hoc syncs
     (held stable for many cycles in practice but not synchronizer chains).
     Functionally safe today because those signals are pulse-then-hold
     much longer than any sane metastability window.

  2. More surgical: `set_false_path` only on the dcfifo's
     `*delayed_wraddr*` → `*rs_dgwp*` (and `*delayed_rdaddr*` → `*ws_dgrp*`)
     register pairs. Correct in principle but fragile w.r.t. dcfifo internal
     hierarchy/naming after Quartus elaboration.

Decision: **document and defer.** The violation is on the Slow 1100mV 85C
corner; Fast corner setup slack on the same domain is +1.85 ns (passing).
The board currently runs functionally at 15 FPS with stable visuals, so the
violation is not breaking real silicon. More importantly, the 2 px/cycle work
in steps 2-4 modifies *only* the `clk` (50 MHz, voxel_gpu user) domain — the
draw pipe, band caches, and edge/recip math. None of it adds combinational
depth in `sdram_ctrl_clk`. So this preexisting violation is orthogonal to the
project's goal and applying SDC fixes in the same pass risks masking real new
violations introduced by steps 2-4. We will revisit after step 4 when post-fit
timing is clearer.

### Step 2: Bank fb_A/B and z_A/B caches via even/odd x

**Decision:** rather than promoting `voxel_sdp_ram` to true dual-port (TDP)
mode of altsyncram, **split each band cache into two half-depth simple-dual-
port banks indexed by `addr[0]`** (= `x[0]`). This is M10K-neutral
(40,960×16 → 2× 20,480×16, same bits per cache) and avoids the ~25% M10K
overhead that TDP mode imposes on Cyclone V (TDP caps at 8 Kb/M10K vs
10 Kb in SDP). M10K headroom matters: post-fit usage was 346/397 = 87%.

Why bank by even/odd x instead of by row, by 16-pixel stripe, etc.:

  * The 2 px/cycle commit pipeline always advances `draw_x_cur` by 2,
    so lane0 commits even x and lane1 commits odd x. Banking the cache
    on the same boundary means both lanes always touch disjoint banks
    and never contend for a write port -- this is the whole point.
  * Reads have the same property because the rasterizer reads
    `pipe0_addr` ahead of the same x stride. The depth-test/blend reads
    naturally split into one-per-bank.
  * Sky/clear paths and SDRAM-driven cache init (linear addr increment)
    just see writes alternating between banks cycle-by-cycle. No
    correctness change at 1 px/cycle.

**Implementation in this step:** new `voxel_banked_sdp_ram` wrapper in
`hw/voxel_gpu/rtl/voxel_sdp_ram.sv` (same file so no qsys filelist
change). External interface is *identical* to `voxel_sdp_ram` (1R/1W) --
this is a drop-in replacement for `fb_back_ram_A/B` and `z_ram_A/B`. The
wrapper:

  * Strips `addr[0]` from rd_addr / wr_addr to form the bank-internal
    address.
  * Routes wr_en to the matching even/odd bank via `(addr[0] == 0)`.
  * Latches `rd_addr[0]` into a 1-cycle register so the bank-select mux
    aligns with the underlying altsyncram's 1-cycle read latency.
  * Picks `rd_data` from `rd_data_even` or `rd_data_odd` accordingly.

The `voxel_sdp_ram` module itself is unchanged -- the wrapper just
instantiates two of them. M10K instance count stays the same per cache
(Quartus packs the two halves into the same M10Ks it would have used for
the unified array).

The texture ROM (`voxel_texture_rom`) is **not** banked here because step
3 promotes it to a true dual-port ROM (different goal: serve two reads
per cycle for the two pixel lanes' UV lookups). The palette table stays
single-port; per-quad it doesn't pay to widen.

**Verification this step:**

  * Verilator lint-only over the wrapper module against an `voxel_sdp_ram`
    stub passes clean (vendor primitive itself remains unverilatable per
    pre-existing project note).
  * No FSM, addr-generator, or pipeline-stage changes anywhere else.
    DRAW_FLUSH_CYCLES stays at 14. The bank-select latch adds zero
    cycles to read latency.

**Risks introduced this step:** none functional. The output mux on
`fb_back_rd_data`/`z_rd_data` is one extra 16-bit 2:1 mux on the read
path, ~0.2-0.4 ns -- negligible vs the 20 ns clock period and orthogonal
to the existing -2.7 ns sdram_pll0 violation (different clock domain).

### Step 3: Promote texture ROM to two independent read ports

**First attempt (rejected by Quartus):** tried to switch the single
`altsyncram` from `operation_mode = "ROM"` to
`operation_mode = "BIDIR_DUAL_PORT"` with both `wren_a` and `wren_b` tied
off, expecting Cyclone V to give us a true 2-read ROM. Quartus 21.1 (or
the IP licence we're using) rejected that pattern with three errors:

  * `272006`: "Must connect clock1 port of altsyncram megafunction when
    using current set of parameters" -- BIDIR_DUAL_PORT refuses to share
    `clock0` for both ports' addresses even when both are read-only.
  * `272006`: "Cannot use different clock ports for address_b port and
    data_b|wren_b|byteena_b(if used) port" -- forces extra clock plumbing
    we don't actually want.
  * `287078`: clear-box licence assertion -- BIDIR_DUAL_PORT on this
    install is gated behind a feature we don't have.

**What actually changed:** `voxel_texture_rom.sv` instantiates **two
parallel `operation_mode = "ROM"` altsyncrams**, both driven by the
same `clock0`, both initialised from the same `voxel_gpu/assets/textures.mif`,
each serving one of the two read ports. Each instance is a standard
single-port ROM (the same shape we already had pre-step-3), so latency,
init pattern, and Quartus-licence requirements are all unchanged.
Per-instance defparams keep `address_aclr_a = "NONE"`,
`outdata_reg_a = "UNREGISTERED"` exactly as the original singleton did, so
the 1-cycle read-latency contract that prevents the chromatic-fringe bug
is preserved on both ports.

The module's external interface gained `rd_addr_b` / `rd_data_b`.
Port A keeps its existing `rd_addr` / `rd_data` names so the rasterizer
instantiation didn't need to rename anything; port B is now driven by the
odd lane's `pipe2_tex_addr_o`.

**Original M10K cost:** atlas was 64 tiles × 16 × 16 × 8 bits = 131 072
bits. Single-port ROM packed ~13 M10Ks; duplicating it doubled that to ~26
M10Ks. The later 128-tile atlas doubles this ROM cost again, so any fit fix
should recover memory outside the raster/cache quality path first.

**Risks:** both ROM copies hold byte-identical data because they share the same
`.mif`, so even/odd lane lookups are coherent. Port B is now used by the
two-lane rasterizer described in step 4.

### Step 4: Duplicate per-pixel datapath into even/odd lanes

May 5 2026 update: this step is now applied in `hw/voxel_gpu/rtl/voxel_gpu.sv`.
The existing unsuffixed per-pixel registers remain lane0/even, and new `_o`
registers carry lane1/odd through the same `pipe0` -> `commit` depth.
`ST_DRAW` now advances `draw_x_cur` by two pixels. The draw start is aligned
down to an even x so lane0 always targets the even cache bank and lane1 always
targets the odd cache bank; if the descriptor's `x_min` is odd, lane0 for the
first pair is simply masked out and lane1 draws the real first pixel.

Texture ROM port B is now driven by `pipe2_tex_addr_o`, and the odd lane has
its own texture, palette-address, palette-result, fog, blend, Z-test, and
commit metadata. The palette table remains the small local array; both lanes
read it in parallel from registered addresses.

**Foundation used by step 4:**

  * `voxel_banked_sdp_ram` (in `hw/voxel_gpu/rtl/voxel_sdp_ram.sv`) now
    exposes per-bank read/write ports over two independent SDP RAMs split by
    `addr[0]`.
  * `voxel_texture_rom` (in `hw/voxel_gpu/rtl/voxel_texture_rom.sv`)
    has `rd_addr_b` / `rd_data_b`, and `voxel_gpu.sv` now routes lane1's texel
    address into that second read port.

**Sub-step 4a: widen `voxel_banked_sdp_ram`**

Replace the unified 1R/1W ports with per-bank 2R/2W:

    rd_addr_e / rd_data_e   (even-x reads,  bank_even)
    rd_addr_o / rd_data_o   (odd-x reads,   bank_odd)
    wr_addr_e / wr_data_e / wr_en_e   (even-x writes, bank_even)
    wr_addr_o / wr_data_o / wr_en_o   (odd-x writes,  bank_odd)

Bank-internal address is `addr[ADDR_W-1:1]`; the caller guarantees
`addr_e[0] == 0` and `addr_o[0] == 1`. Update each of the four cache
instantiations in `voxel_gpu.sv` (the `fb_back_ram_A/B` and `z_ram_A/B`
blocks following the comment "Ping-pong band caches A and B"). The cache
port driver around lines 1846-1916 today produces a single triple
(addr, data, en); rewrite it to produce a pair of (addr_e, data_e, en_e)
and (addr_o, data_o, en_o). For single-pixel paths (`ST_CLEAR`,
`ST_FETCH` sky prime, `ST_CACHE_INIT`, `ST_CACHE_LOAD_*`), drive both
ports with the same address and value and qualify each `wr_en` by the
linear address's LSB. For `ST_DRAW`, lane0's commit feeds `wr_addr_e`
and lane1's feeds `wr_addr_o` directly.

**Sub-step 4b: duplicate per-pixel state in `ST_DRAW`**

Every stage from `pipe0_*` through `commit_*` now has two lanes. The original
unsuffixed registers are lane0/even; new `_o` registers are lane1/odd. The
even lane evaluates at `(x, y)` and the odd lane at `(x+1, y)`. Edge values,
Z, UV/perspective increments, and fog radial metadata arrive at lane1 with the
lane0 + dx-step adjustment.

Applied diff shape:

  * Kept the old unsuffixed pipe as lane0/even instead of doing a rename-only
    pass.
  * Added `_o` clones for lane1/odd.
  * Lane1 receives edge/Z/UV/IW values equal to lane0 + the appropriate dx
    step.
  * Lane0 captures even-bank color/Z read data and lane1 captures odd-bank
    color/Z read data.
  * Both lanes can commit in the same cycle through `wr_addr_e`/`wr_data_e`
    and `wr_addr_o`/`wr_data_o`.

**Sub-step 4c: ST_DRAW FSM (`voxel_gpu.sv` ~lines 3240-3303)**

  * `draw_x_cur <= draw_x_cur + 10'd2` instead of `+ 10'd1`.
  * The end-of-row test `(draw_x_cur == draw_x_max)` becomes
    `(draw_x_cur >= draw_x_max - 1)`. For odd-width quads (last pixel in
    lane0 only, lane1 outside the bbox), gate lane1's `commit_valid` by
    `(draw_x_cur + 1 <= draw_x_max)`; lane1 still walks the pipeline but
    is masked out at commit, the same way `pipe0_inside` masks
    outside-edge pixels today.
  * The early-exit on inside-to-outside transition currently checks
    `(draw_row_inside && !draw_inside)` (~line 3266). With two lanes we
    examine `draw_inside_e` and `draw_inside_o`: row exits only when
    BOTH lanes have left the triangle interior, since either lane could
    be the trailing pixel of an odd-aligned quad.

**Sub-step 4d: pipeline drain (`DRAW_FLUSH_CYCLES`, line 96)**

The 14-cycle drain stays at 14 -- the lane1 pipeline shares depth with
lane0. The `*_valid` flop in each stage just becomes a 2-bit pair
{`*_valid_e`, `*_valid_o`}. End of `ST_DRAW` waits until both lanes have
drained.

**Cache writes during commit (depends on 4a + 4b.3):**

When both lanes commit a pixel: lane0 writes `commit_addr_e` (always
even) into the even bank's write port, lane1 writes `commit_addr_o` (=
even+1, always odd) into the odd bank's port. Z bank: same pattern.
Single-lane commit (last pixel of an odd-width row): only the lane0
write port fires; the odd bank's `wr_en_o = 0`.

**Verification gates between 4a and 4b/4c:**

After 4a (wrapper widening + cache port rewrite, but still 1 px/cycle
draw), the visuals should be unchanged. A board run plus a frame-hash
diff against the pre-4a build is a cheap regression check. After
4b/4c, the renderer is at 2 px/cycle and frame timing should drop by
~40-50% on draw-bound workloads (sky/grass world view with hotbar ratio
constant).

May 5 2026 local verification after applying 4b/4c/4d:

  * `git diff --check` passed.
  * `verilator --lint-only --bbox-unsup ... voxel_gpu.sv ...` passed.
  * Quartus/board validation still needs to be run on the build machine.

**Resource budget estimate (Cyclone V, post-fit baseline 25,997/32,070
ALMs at 81%, 346/397 M10Ks at 87%):**

  * ALMs: +4-7k for the duplicated per-pixel datapath (edge eval, recip,
    UV, fog math). Tight given 6,073 free.
  * M10Ks: +0 from cache banking, +~13 from duplicating the texture ROM
    (two ROM-mode altsyncrams sharing one `.mif`). Within 51 free pre-fit.
  * DSPs: +0 to +12 depending on whether Quartus shares multipliers
    across lanes. Plenty of headroom (30/87 today).
  * Fmax: same 50 MHz target. The added combinational load is wide but
    shallow (more 32/64-bit adders in parallel, not a longer chain).

If the post-4a fit shows the SDRAM PLL setup violation drifting
(currently -2.7 ns), revisit the deferred SDC false-path fix from step 1
before adding the lane1 pipeline.

### May 5 2026: Post-2px board timing and current blocker

After the full 2 px/cycle rasterizer build, visuals are reported good but
frame rate is mostly unchanged. The new software diagnostics show the useful
distinction: the per-pixel raster workload is no longer the obvious blocker,
but frame completion is still dominated by band submission synchronization,
FIFO backpressure, final flush, vsync waiting, and CPU-side scene/text setup.

Captured target run:

    renderer: HW mode [flat buf, 3147 quads, bbox: (0,0)-(639,479)] bin= 1.62ms bands=25.76ms
    renderer: band detail b0= 0.90( 0.01/ 0.15/ 0.74,6720B) b1= 0.83( 0.00/ 0.01/ 0.82,320B) b2= 5.58( 0.00/ 0.01/ 5.56,448B) b3= 3.00( 0.00/ 0.01/ 2.98,576B) b4= 4.25( 0.00/ 0.01/ 4.24,320B) b5= 3.77( 0.00/ 0.01/ 3.75,320B) b6= 4.94( 0.00/ 4.63/ 0.30,100928B) b7= 2.49( 0.00/ 2.27/ 0.22,97280B)
    renderer: calc desc=1677 copies=1734 tex=1499 sky_skip=30 bbox1= 2.48ms bbox2= 1.30ms save= 1.18ms init= 6.14ms fetch= 1.03ms overhead= 0.58ms flush_raw= 6.14ms flush_slack= 7.52ms ideal_ready=10.42ms vsync_floor=16.80ms slots=1
    renderer: FLIP flip=21.10ms
    perf: fps= 19.6 frame= 51.00ms work= 50.91ms update= 0.04ms begin= 0.01ms draw= 13.74ms end= 37.13ms sleep= 0.00ms max_work= 62.24ms quads=1719.6 sky=25.0 phys= 2.9

Interpretation:

  * `end=` in `game.c` is `renderer_end_frame()`: binned hardware submit
    plus `FLIP`. It is not just the final ioctl.
  * The diagnostic model estimates only 1.30 ms of two-lane bbox raster work
    for this frame. That means the 2 px/cycle implementation did its narrow
    job; the remaining problem is system-level pacing.
  * `bands=25.76ms` is already too high. Bands 6 and 7 spend most of their
    time in the descriptor write phase (`4.63ms` and `2.27ms`) because they
    carry ~100 KB each of descriptor data. That is FIFO backpressure from the
    hardware consuming descriptors/raster work slower than userspace can
    stream it.
  * Bands 2-5 have tiny descriptor payloads but large `END_BAND` waits, so
    those bands are waiting for the hardware to become idle after their draw
    pass. That points at synchronization/cache/flush ordering, not CPU binning.
  * `FLIP=21.10ms` means the final wait is significant too: it includes
    waiting for FIFO empty, engine idle, requesting flip, and then waiting for
    the next `VSY` latch.
  * `draw=13.74ms` is CPU-side scene/HUD construction before descriptors are
    submitted. With the 30 FPS target, the whole frame must land inside the
    2-vsync bucket: `2 * 16.8ms = 33.6ms`. A 13.7 ms CPU draw leaves only
    about 19.9 ms for all submit/flip work; the captured submit+flip is
    ~37 ms, so both CPU draw cost and hardware wait cost matter.

Vsync bucket math:

  * VGA timing is 50 MHz, 1600 cycles/line, 525 lines/frame:
    840000 cycles = 16.8 ms = 59.52 Hz.
  * 30 FPS corresponds to the 2-vsync bucket: 33.6 ms.
  * 20 FPS corresponds to the 3-vsync bucket: 50.4 ms.
  * 15 FPS corresponds to the 4-vsync bucket: 67.2 ms.
  * Therefore the observed "20 FPS, then after a few seconds 15 FPS" is
    consistent with work occasionally crossing from the 3-vsync bucket into
    the 4-vsync bucket, not with a smooth linear slowdown.

Fixed-cost lower bounds under the current SDRAM scanout policy:

  * Full local band cache init/clear is 307200 cycles = 6.144 ms at 50 MHz.
  * Full-frame color flush is 307200 words. Raw one-word-per-cycle push is
    6.144 ms.
  * Because active display owns part of SDRAM time, writes are allowed during
    blanking plus the first 960 cycles of each visible line:
    225600 + 460800 = 686400 allowed cycles per 840000-cycle VGA frame.
    That makes a full-frame flush about 7.52 ms wall-clock in the simple
    one-word-per-allowed-cycle model.

Diagnostics currently available:

  * `VOXEL_DIAG_BBOX=1` prints the binned bbox/model line once per 60 frames.
  * The `band detail` tuple is `total(begin/write/end,bytes)`.
  * Interpretation guide:
    - large `begin` = waiting for prior background flush/cache availability
      before `BEGIN_BAND`.
    - large `write` = FIFO backpressure while streaming descriptors.
    - large `end` = waiting for FIFO empty and hardware idle before issuing
      `END_BAND`.
    - large `FLIP` = final flush/idle/vsync wait.

Immediate software change:

  * `sw/chat.c` now merges contiguous lit pixels in each glyph row into one
    rectangle before calling `renderer_fill_rect()`. This preserves exact
    appearance but reduces text descriptor count.
  * Calculated examples with the 5x7 font:
    - `"fps 19.6"`: 88 per-pixel rects -> 50 row-run rects, 43.2% fewer.
    - `"fps 15.0"`: 96 -> 55, 42.7% fewer.
    - `"mode=survival"`: 191 -> 132, 30.9% fewer.
  * This is relevant because the FPS overlay is not drawn until the first perf
    window has produced `fps_text_len`; that timing matches the report that
    FPS starts near 20 and drops to 15 after a few seconds. The fix is not a
    quality reduction and should reduce both CPU `draw=` time and descriptor
    pressure from HUD/chat text.
  * `sw/renderer.c` now performs a conservative per-face camera-frustum reject
    after the face's four camera-space vertices are computed and before
    projection, clipping, LOD choice, UV-plane fit, and descriptor packing.
    This rejects only faces whose entire quad is outside the same near/left/
    right/top/bottom clip plane, so it should not change visible quality. The
    reason is that chunk-level frustum culling is necessarily coarse: a visible
    chunk can still contain many merged faces wholly off-screen, and those were
    previously paying full CPU descriptor setup cost and sometimes adding
    descriptor traffic before the viewport clipper rejected them.

Current next hypotheses to test:

  * If the next run still drops after the first FPS label appears, compare
    diagnostics with the overlay disabled or with only one `chat_draw_text()`
    call for FPS. That would confirm or rule out text/HUD descriptor pressure.
  * If `band detail` still shows large `write` on bands 6/7, optimize world
    descriptor volume next: better per-chunk/frustum face rejection, cheaper
    screen-space rejection before descriptor generation, or more aggressive
    far-face merging without changing visual quality.
  * If `FLIP` remains >16.8 ms, investigate whether the final background flush
    is missing the current vsync and should be requested earlier or tracked
    separately from display flip.

### May 5 2026: Updated 20 FPS logs and first RTL-side speedup

New board logs after the text-row-run and software face-frustum changes:

    perf: fps= 19.8 frame= 50.40ms work= 50.31ms update= 0.03ms begin= 0.01ms draw= 13.10ms end= 37.17ms sleep= 0.00ms max_work= 50.35ms quads=1521.0 sky=25.0 phys= 3.0
    renderer: HW mode [flat buf, 3269 quads, bbox: (0,0)-(639,479)] bin= 0.85ms bands=28.10ms
    renderer: band detail b0= 0.90( 0.01/ 0.13/ 0.77,6976B) b1= 0.83( 0.00/ 0.01/ 0.81,320B) b2= 2.75( 0.00/ 0.01/ 2.74,576B) b3= 2.81( 0.00/ 0.01/ 2.80,576B) b4= 5.65( 0.04/ 0.05/ 5.56,2624B) b5= 8.48( 0.00/ 8.36/ 0.11,169152B) b6= 2.49( 0.00/ 1.88/ 0.61,37952B) b7= 4.17( 0.00/ 0.10/ 4.06,5888B)
    renderer: calc desc=1740 copies=1870 tex=1631 sky_skip=30 bbox1= 7.07ms bbox2= 3.60ms save= 3.47ms init= 6.14ms fetch= 1.12ms overhead= 0.63ms flush_raw= 6.14ms flush_slack= 7.52ms ideal_ready=12.41ms vsync_floor=16.80ms slots=1
    renderer: FLIP flip= 7.90ms

Current bottleneck:

  * This is still exactly the 3-vsync bucket: `3 * 16.8ms = 50.4ms`.
  * To reach the 30 FPS cap, total work must be under the 2-vsync bucket:
    `33.6ms`. With `draw=13.10ms`, `update=0.03ms`, and `begin=0.01ms`,
    `renderer_end_frame()` needs to be roughly <=20.45ms. It is currently
    ~37.17ms, so we still need about 16.7ms of total frame savings.
  * The latest `end` breaks down as roughly:
    - `bin=0.85ms`
    - `bands=28.10ms`
    - `FLIP=7.90ms`
  * Total descriptor payload across the eight bands in this diagnostic frame
    is 224064 bytes. The measured write phases total about 10.55ms, only
    ~21 MB/s effective userspace -> kernel -> FPGA FIFO throughput. That means
    descriptor byte volume and PIO/MMIO bandwidth are now real bottlenecks.
  * Band 5 is the smoking gun for bad camera angles: it contains 169152 bytes
    of descriptors and spends 8.36ms in the write phase. Looking high/down can
    concentrate projected terrain into one or two screen bands, creating exactly
    this kind of descriptor pile. If it crosses the next vsync boundary, FPS
    falls from 19.8 to ~14.9 (4-vsync bucket), or even ~11.9 (5-vsync bucket).

RTL-side speedup applied:

  * `ST_CACHE_INIT` now uses the even/odd banked local cache write ports to
    initialize two pixels per 50 MHz cycle instead of one.
  * This affects both color-cache init and z-cache clear for every band.
  * Full-frame local cache init/clear estimate changes from:
    - old: 307200 cycles = 6.144ms
    - new: 153600 cycles = 3.072ms
    - expected fixed saving: ~3.07ms/frame
  * `sw/gpu_transport.c` diagnostic math was updated so future `init=...`
    estimates match the two-pixel init path.
  * This should reduce some of the small-payload band `END_BAND` waits, but it
    is not enough alone to reach 30 FPS; the remaining target is still on the
    order of 13ms+.

RTL-side options considered next:

  * Faster main GPU clock is not a simple switch. The current 50 MHz clock also
    drives VGA timing expectations and the Avalon-facing control/FIFO logic.
    Timing reports show theoretical Fmax above 50 MHz for this domain, but
    running raster/cache at a faster independent clock would require careful
    clock-domain crossings around the FIFO, scanout/cache arbitration, and
    SDRAM paths. It is possible as a larger architecture change, not a quick
    safe patch.
  * Increasing the active-line SDRAM write window (`ACTIVE_WRITE_END_HCOUNT`)
    could recover under ~1ms of flush slack, but it risks reintroducing scanout
    starvation/flash artifacts. Treat as a cautious tuning knob only after
    bigger descriptor-volume wins.
  * Wider descriptor ingestion or DMA would attack the measured ~21 MB/s PIO
    bottleneck, but this is a driver/RTL interface change. A deeper FIFO would
    mostly move time from `write` to `END_BAND` unless the hardware also drains
    descriptors faster.
  * More raster lanes alone are not attractive now: `bbox2=3.60ms` says pixel
    pair work is not the dominant term in this captured frame.

HPS-to-FPGA bridge bandwidth note:

  * `hw/soc_system.qsys` currently maps `fpga_sdram.s1` behind the full
    `hps_0.h2f_axi_master`, but maps `voxel_gpu_0.avalon_slave_0` behind
    `hps_0.h2f_lw_axi_master`.
  * Therefore the descriptor FIFO/register aperture used by `/dev/voxel_gpu`
    is still on the lightweight HPS-to-FPGA bridge, while the separate SDRAM
    test window is on the full bridge.
  * Moving the GPU slave to the full H2F bridge is possible and is probably the
    lowest-risk bus-bandwidth experiment: reconnect the Platform Designer/Qsys
    Avalon connection from `h2f_lw_axi_master` to `h2f_axi_master`, regenerate
    the system/device tree, and make sure the device-tree `reg` points at the
    new full-bridge physical aperture. The kernel driver already maps the OF
    resource, so the userspace ABI and descriptor format should not need to
    change.
  * Caveat: the GPU slave itself is still a 32-bit PIO FIFO window. The full
    bridge can remove lightweight-bridge bandwidth/transaction limits, but it
    will not magically become a wide burst DMA path unless the RTL/driver
    protocol is also widened or replaced with a DMA/ring-buffer style command
    uploader. Treat this as a useful intermediate step, not the final maximum-
    bandwidth architecture.

Bridge-blocker verification math:

  * The Avalon slave has no `waitrequest`; FIFO backpressure is implemented in
    `sw/voxel_gpu.c` by polling `STATUS.FIFO_COUNT` before each
    `iowrite32_rep()` burst. Therefore `band write` time must be split into:
    - time waiting for FIFO space (`voxel_fifo_wait_space`)
    - time actually issuing MMIO writes (`iowrite32_rep`)
  * If `iowrite32_rep` dominates, the HPS bridge / Linux MMIO path is the
    blocker. If FIFO-space wait dominates, the FPGA descriptor/raster pipeline
    is the blocker and a faster bridge mostly helps only short bursts.
  * Latest measured payload:
    - total descriptor bytes: 224064 B
    - summed band write time: about 10.55 ms
    - effective upload rate: `224064 / 0.01055 = 21.2 MB/s`
  * Ideal lower bound for the current 32-bit, 50 MHz slave if it accepted one
    word every cycle:
    - bandwidth: `4 B * 50 MHz = 200 MB/s`
    - transfer time: `224064 / 200e6 = 1.12 ms`
  * Therefore the absolute best same-width bus-side saving on that frame is
    only about `10.55 - 1.12 = 9.43 ms`. Since the frame still needs roughly
    16.7 ms of savings to move from the 3-vsync bucket (50.4 ms) to the
    2-vsync bucket (33.6 ms), the bridge cannot be the only remaining 30 FPS
    blocker in that captured frame.
  * However, the bridge can explain whole-vsync drops on descriptor-heavy
    camera angles. Approximate one-vsync saving threshold:
    `bytes * (1/21.2e6 - 1/200e6) >= 16.8ms`, so bytes must be about
    `400 KB` before an idealized bridge-speed fix can recover a full 60 Hz
    vsync slot by itself.
  * `sw/voxel_gpu.c` now has optional `diag_upload=1` module instrumentation
    that accumulates one-second upload timing splits:
    - FIFO-space wait time (`voxel_fifo_wait_space`)
    - actual MMIO push time (`iowrite32_rep`)
    - total bytes, bursts, total effective KiB/s, and MMIO-only KiB/s
    Enable with `insmod voxel_gpu.ko diag_upload=1` or by writing `1` to the
    module parameter in sysfs if the module is already loaded.
  * The local Mac-side environment cannot compile the kernel module because it
    has no Linux kernel build tree under `/lib/modules/.../build`; compile this
    check on the DE1/Linux side.

May 5 upload-diagnostic run, pasted game log:

  * Stable ground/block-heavy view remains in the same 3-vsync bucket:
    `fps=19.8`, `frame=50.40ms`, `work=50.31ms`.
  * Breakdown:
    - `draw=13.15ms`
    - `end=37.12ms`
    - `bin=0.89ms`
    - `bands=28.21ms`
    - `FLIP=7.73ms`
  * Descriptor payload is 224192 B. Summed band `write` time is about
    10.69 ms, so effective descriptor-upload-plus-backpressure rate is about
    `224192 / 0.01069 = 21.0 MB/s`, essentially unchanged from the prior
    capture.
  * The largest descriptor pile is still band 5:
    `169152B`, `write=8.48ms`.
  * Summed band `END_BAND` wait time is about 17.46 ms, larger than the
    descriptor write total. This continues to point at both descriptor traffic
    and hardware/flush synchronization, not just raw bridge bandwidth.
  * This pasted log did not include the new kernel `voxel_gpu: upload diag:`
    lines, so it does not yet split `write` into FIFO-space wait versus actual
    `iowrite32_rep()` MMIO time. Need the `dmesg` lines to decide whether the
    lightweight bridge is the primary blocker.

May 5 upload-diagnostic kernel split:

    voxel_gpu: upload diag: calls=640 bytes=4483840 bursts=1460 wait=131575us(63%) mmio=74396us(36%) rate_total=21258KiB/s rate_mmio=58856KiB/s

Steady-state readout:

  * The actual MMIO/bridge push path is about 58-59 MiB/s.
  * The total effective descriptor-write section is still about 21 MiB/s
    because FIFO-space waiting dominates: roughly 63% wait versus 36% MMIO.
  * At 19.8 FPS, this line is approximately:
    - descriptor bytes per frame: `4483840 / 19.8 = 226 KB/frame`
    - FIFO-space wait per frame: `131.6ms / 19.8 = 6.6ms/frame`
    - MMIO push per frame: `74.4ms / 19.8 = 3.8ms/frame`
    - upload section per frame: about 10.4ms/frame
  * Therefore moving the GPU aperture to the full H2F bridge can at most attack
    the ~3.8ms/frame MMIO piece in this stable capture. It cannot remove the
    ~6.6ms/frame FIFO wait, and it still does not close the full ~16.7ms gap
    from the 3-vsync bucket to the 2-vsync/30-FPS bucket.
  * Conclusion: the lightweight bridge is a real secondary cost, but the
    primary steady-state upload blocker is FPGA-side descriptor consumption /
    command-FIFO backpressure. Prioritize reducing descriptor bytes, increasing
    descriptor consume rate, or allowing descriptor upload to overlap more
    non-consuming phases before spending the next risky hardware iteration on
    bridge relocation alone.
  * RTL reason: the command FIFO is only popped by `fifo_pop_req` in
    `ST_FETCH`. It can pop at most one 32-bit word per 50 MHz cycle while that
    state is active, which is already a 200 MB/s theoretical read rate. The
    measured descriptor payload of ~224 KB/frame would take only ~1.1 ms to
    read if `ST_FETCH` ran continuously. The FIFO fills because the main FSM
    stops popping while it executes the fetched descriptor (`ST_DRAW` /
    `ST_DRAW_FLUSH`) and while band/cache/SDRAM phases run (`ST_CACHE_INIT`,
    load/drain, queued flush interactions, etc.).
  * Therefore the immediate FPGA-side slowdown is not literal FIFO read
    bandwidth. It is serialized descriptor execution and band maintenance:
    fetch one descriptor, rasterize/skip it, flush its pipeline, then fetch the
    next descriptor. `renderer: calc fetch=~1.12ms` versus `bbox2=~3.6ms`,
    `init=~3.1ms`, `flush_raw=~6.1ms`, and `flush_slack=~7.5ms` matches this.
  * Parallelizing descriptor *fetch* alone would mostly reduce the ~1.1 ms
    fetch term. The more useful and feasible intermediate RTL idea is a small
    decoded-descriptor prefetch queue: keep popping command FIFO words into one
    or two decoded descriptor slots while `ST_DRAW` is busy, then start the
    next descriptor without re-entering a long serial fetch phase. True
    parallel descriptor *execution* is much bigger because two rasterizers
    would contend for the same z/color cache writes and must preserve Z/alpha
    ordering for overlapping quads.
  * Why the 2-pixel raster lane did not move FPS much: it only attacks the
    bbox/pixel-march term. In the stable log, the model says the old 1 px/cycle
    bbox work would be ~7.07 ms and the new 2 px/cycle bbox work is ~3.60 ms,
    a ~3.47 ms saving. But the frame is still stuck in the 50.4 ms / 3-vsync
    bucket until total work drops below 33.6 ms. The remaining large terms are
    CPU `draw` (~13 ms), FIFO/upload blocking (~10 ms split across wait+MMIO),
    band flush/init/synchronization (~17 ms of `END_BAND` waits), and `FLIP`
    (~7-8 ms).
  * Rasterizer-side improvements that still make sense:
    - reduce bbox area rather than adding more lanes: row-span rasterization or
      per-row x-start/x-end would skip outside-left/outside-right bbox pixels
      instead of walking each descriptor's full bounding box;
    - add descriptor prefetch/decode queueing to hide the ~1.1 ms fetch phase
      and reduce command-FIFO backpressure;
    - create specialized fast paths for common opaque flat block faces that do
      not need perspective texture setup, alpha, fog, or expensive per-pixel
      work;
    - reduce/overlap band maintenance (`ST_CACHE_INIT`, SDRAM flush/drain, and
      scanout-safe slack), since those costs are now comparable to or larger
      than the pixel march itself.
  * Note: `renderer: calc init=3.07ms` reflects updated software diagnostic
    math for the pending two-pixel cache-init RTL. If the matching RBF was not
    rebuilt, this is an estimate only; the measured `band detail`/`perf` lines
    are the authoritative data.

Near-term best target:

  * Reduce descriptor byte volume and per-frame CPU descriptor setup. The
    worst cases are the camera angles where one screen band receives a huge
    textured-descriptor pile. The strongest quality-preserving path is better
    world/face culling and/or more compact descriptor formats for common world
    faces, because that reduces CPU `draw=`, PIO `write=`, FIFO pressure, and
    hardware fetch overhead together.

Concrete path to a reliable 30 FPS+ budget:

  * Stable frame budget today is about `work=50.3ms`. To hit the software
    30 FPS cap, work must land below the 2-vsync bucket (`33.6ms`); for margin,
    target ~28-30ms. That means we need roughly 17ms of savings just to cross
    the threshold and ~20ms+ to be comfortable.
  * Do not expect one fix to carry this. The current largest stable-frame
    contributors are:
    - CPU `draw`: ~13ms
    - descriptor upload section: ~10.4ms/frame, split ~6.6ms FIFO wait and
      ~3.8ms MMIO push
    - band `END_BAND` waits / flush synchronization: ~17.5ms
    - `FLIP` wait: ~7-8ms
  * Recommended order:
    1. Build/test the existing two-pixel `ST_CACHE_INIT` RTL. If the current
       RBF does not include it, this should recover about 3ms/frame.
    2. Add a software-side “dirty band” / empty-band skip so bands with no
       non-sky descriptors do not pay begin/init/end/flush work. This directly
       attacks `END_BAND`, `FLIP`, and bad sky/ground cases.
    3. Reduce descriptor bytes and CPU draw together: merge/specialize common
       block-face descriptors or add stronger screen/band culling before
       descriptor emission. Every descriptor removed saves CPU setup, MMIO
       upload, FIFO wait, fetch, and raster work.
    4. Add an RTL decoded-descriptor prefetch queue only after byte/empty-band
       cleanup. It can hide the ~1.1ms fetch term and reduce FIFO fullness, but
       it is not a 17ms fix by itself.
    5. Treat full H2F bridge relocation as a secondary improvement. Current
       diagnostics say it can only attack the ~3.8ms/frame MMIO portion in the
       stable capture, not the larger FIFO-wait/flush costs.

May 5 async-present / cheap-sky software patch:

  * The two-pixel cache-init RTL still failed to fit after minor logic shaving:
    Quartus reported 3251 required LABs versus 3207 available. Park this path
    unless we free area elsewhere.
  * Added `VOXEL_IOC_FLIP_ASYNC` and `VOXEL_IOC_WAIT_FLIP` to the kernel ABI.
    The old `VOXEL_IOC_FLIP` remains the blocking compatibility path.
  * `gpu_transport_flip()` now requests a hardware flip asynchronously when the
    new driver supports it, while `gpu_transport_clear()` waits for any pending
    flip before clearing the newly inactive framebuffer. This moves the vsync
    wait out of the previous frame's `end_frame()` and lets CPU descriptor
    construction for the next frame overlap most/all of the present wait.
  * `renderer_begin_frame()` no longer clears hardware. It only resets the CPU
    staging buffer. `renderer_end_frame()` waits for the previous async flip,
    clears the inactive frame, flushes deferred GPU state, submits descriptors,
    then requests the next async flip.
  * Dynamic palette/fog writes are now queued in `RenderContext` and flushed
    only after the pending flip wait and clear. This preserves correctness for
    generated-sky SDRAM flushes: the next frame no longer changes palette state
    while the previous frame may still be finishing its present/flush path.
  * Cheap-path improvement: the hardware already paints the sky gradient during
    band init when `VOXEL_EXTMEM_CTRL_SKY_GRADIENT_CLEAR` is enabled, so
    full-width sky-gradient descriptors are omitted from the hardware FIFO
    stream. This preserves the generated sky visual but avoids redundant
    descriptor bytes/fetch/overhead on cheap sky bands. Socket/virtual output
    still receives the original descriptor stream.
  * Expected log changes:
    - `renderer: FLIP flip=...` may become `renderer: FLIP wait=...`.
    - If overlap works, `FLIP wait` should be near zero on steady frames
      because the vsync completed while the CPU was drawing the next frame.
    - `sky_skip` should still count omitted sky-gradient copies, while band
      byte totals should drop by the omitted sky descriptors.
  * Verification done locally:
    - `git diff --check` passed.
    - `cc -O2 -Wall -Wextra -I sw -fsyntax-only sw/gpu_transport.c
      sw/renderer.c sw/chat.c sw/pause_menu.c sw/game.c` passed.
    - Renderer test binaries build; only existing `world.c` warnings remain.
    - Kernel module compile must be done on the DE1/Linux environment.

May 5 fitter result for two-pixel `ST_CACHE_INIT`:

  * Quartus failed in fitter placement preparation:
    - required LABs: 3250
    - available LABs: 3207
    - over by 43 LABs (~1.3%)
  * This is a resource fit failure, not a timing failure. Because the design is
    barely over, very small logic reductions can matter.
  * The two-pixel init patch was tightened after this report:
    - odd-bank init address now wires the low bit to 1 instead of using a
      16-bit `+ 1` adder;
    - odd-bank init write enables are unconditional because every band pixel
      count is even and `cache_maint_addr` advances by 2;
    - terminal checks use equality instead of broader `>=` comparisons where
      the FSM already guarantees even stepping.
  * Verilator lint/syntax still passes locally after the resource-shaving
    change. Quartus fitting still needs to be retried on the build machine.

May 5 follow-up from async-present test logs:

  * The async-present / sky-descriptor omission patch did reduce command bytes:
    primer-only bands now show up as `64B` instead of carrying all full-width
    sky-gradient descriptors. However, stable ground view was still stuck in
    the 3-vsync bucket:
    - `fps=19.8`
    - `frame/work ~= 50.4ms`
    - `draw ~= 13.1ms`
    - `end ~= 37.1ms`
    - `FLIP wait ~= 6.45ms`
    - `bands ~= 29.7ms`
  * The critical observation is that a primer-only band can still cost nearly
    a millisecond, and in sky/ground views several primer-only bands had been
    costing multiple milliseconds. That means the cheap path cannot only remove
    descriptor bytes; it must avoid the whole `BEGIN_BAND` / `END_BAND` /
    generated-sky flush transaction when the inactive SDRAM framebuffer already
    contains the same sky-only band.
  * Added a software-side safe band-reuse cache in `sw/gpu_transport.c`:
    - Track a generated-sky palette epoch for palette entries 40..63.
    - Track `sky_band_epoch[physical_buffer][band]` for the two SDRAM color
      buffers.
    - Decode `copy_target_sel` from `VOXEL_IOC_GET_EXTMEM.dma_status` bit 11
      after `CLEAR_FRAME`, so the transport knows which physical SDRAM buffer
      is being refreshed this frame.
    - If a band is primer-only after redundant sky descriptors are omitted, and
      the target buffer's band already has the current sky epoch, skip the whole
      hardware band submit. Otherwise submit it normally, then mark that
      physical buffer's band valid for the current epoch.
    - Any real descriptor in a band invalidates that target buffer's sky-only
      validity for that band.
  * New diagnostic field: `sky_band_reuse=N` in the `renderer: calc ...` line.
    A reused band should show `0B` and `0.00/0.00/0.00` in `band detail`.
  * Expected impact:
    - Stable block/ground view may still remain around 20 FPS because only one
      band is primer-only in the latest pasted log.
    - Looking mostly at sky/ground should improve more, because those views can
      produce multiple primer-only bands after sky descriptor omission.
    - This is a quality-preserving skip: it only reuses a band when the same
      physical SDRAM buffer already holds the generated sky for the current
      sky-palette epoch.

May 5 follow-up from safe sky-band reuse test logs:

  * The safe sky-band reuse patch worked but was too narrow for the stable
    ground view:
    - `sky_band_reuse=1`
    - band 1 showed `0.00(...,0B)`
    - steady `bands` improved from about `29.7ms` to about `26.9ms`
    - kernel upload wait improved from about `131ms/s` to about `106ms/s`
    - dmesg total upload rate improved from about `21.2MiB/s` to about
      `24.1MiB/s`
  * That still leaves the frame in the 3-vsync bucket (`~50.4ms`, `~19.8 FPS`)
    because only one stable-view band is truly sky-only. The remaining cost is
    real geometry and UI bands, especially:
    - `b5 ~= 168896B`, with `write ~= 7.1ms`
    - several small real bands still paying multi-ms `END_BAND` waits
    - `FLIP wait ~= 9.1ms` because the frame still misses the 2-vsync deadline
  * Added an exact per-band SDRAM reuse cache in `sw/gpu_transport.c`:
    - hash each binned post-sky-omission band byte stream;
    - track `{hash, bytes, render_epoch}` per physical SDRAM buffer and band;
    - increment `render_epoch` on hardware palette/fog writes, since identical
      descriptors can produce different pixels after a palette/fog change;
    - if the target buffer already contains the same band for the current
      render epoch, skip the whole hardware `BEGIN_BAND`/upload/`END_BAND`
      transaction.
  * New diagnostic field: `band_reuse=N` in addition to `sky_band_reuse=N`.
    For a stationary camera, after both physical SDRAM buffers have been
    populated, repeated geometry-heavy bands should start reporting
    `band_reuse` and show `0B` in `band detail`.
  * This is not a moving-camera cure; if descriptors change every frame, this
    cache will miss. It is meant to stop wasting 20 FPS worth of work while the
    view is static or nearly static, and to confirm how much of the remaining
    problem is repeated identical band work versus unavoidable new geometry.

May 5 moving-camera performance plan:

  * The latest STA summary is:
    - main voxel/VGA/user clock domain Fmax: `59.52MHz`
    - SDRAM controller PLL domain Fmax: `117.86MHz`
  * The main design currently runs at `50MHz`. In the stable geometry-heavy
    logs, CPU `draw` is about `13.1ms` and hardware `end` is about `37.2ms`.
    To hit the 30 FPS cap, total work must be below the 2-vsync bucket
    (`33.6ms`), so `end` needs to be roughly `20ms` or less.
  * Clocking the existing single-clock design faster is not enough:
    - best-case 50 -> 59.52MHz only scales hardware work by `50/59.52 = 0.84`
    - `37.2ms * 0.84 ~= 31.2ms`
    - `13.1ms draw + 31.2ms end ~= 44.3ms`, still above the 2-vsync bucket
  * It is also not a simple PLL tweak because `voxel_vga_counters` derives VGA
    timing from the same `clk` and drives `VGA_CLK = ~hcount[0]`. Raising that
    clock directly changes the VGA pixel clock and line/frame timing. A real
    overclock path means separate clock domains: keep VGA/scanout timing at
    50MHz and run raster/cache/SDRAM-facing work on a faster GPU clock with
    explicit CDC around command FIFO, scanout line fill, and status/control.
  * Concrete moving-camera improvements, in recommended order:
    1. **Enable/test greedy merging for rendered chunks.** `world.c` uses
       `NEAR_CHUNK_RADIUS=3`, and render distance is 3 chunks, so almost every
       rendered chunk near the player bypasses greedy merging. Lowering this to
       1 or making it runtime-configurable should drastically reduce world face
       descriptors without changing textures or geometry; the known risk is
       close-range T-junction/UV shimmer.
    2. **Replace physical band clear/init with valid bits.** Instead of writing
       every color/Z cache entry during `ST_CACHE_INIT`, clear a compact valid
       bitmap for the band. Invalid pixels read as sky color and far Z; commit
       sets valid. This attacks the current `init ~= 2.7-3.1ms` cost with a
       much smaller clear and should be less area-hungry than the failed
       two-pixel cache-init patch.
    3. **Improve SDRAM flush arbitration / scanout buffering.** Full-frame band
       flush is still effectively slack-limited (`flush_slack ~= 6.5-7.5ms`).
       A deeper scanout prefetch/line buffer or watermark scheduler could let
       background writes use more active-display cycles without starving VGA.
    4. **Compact textured descriptors or add a decoded-descriptor prefetch
       queue.** Fetch itself is only about `1.1ms`, but descriptor bytes create
       FIFO backpressure. A compact textured descriptor or prefetch/decode queue
       helps upload pressure; it is secondary to reducing the number of world
       faces.
    5. **Move the GPU aperture to full H2F.** This can reduce the `~3.7ms/frame`
       MMIO portion, but current dmesg still shows FIFO-space wait dominating,
       so it will not close the 30 FPS gap alone.

May 5 follow-up — sky-band reuse skip cache wasn't hitting:

  * Symptom: with sky-only/ground-only views the `sky_band_reuse=N` counter
    stayed at 0, and per-band `init`/`flush` cost was being paid every frame
    even when nothing visible had changed. CPU `draw` had also dropped from
    `13.1ms` to about `5-7ms` after `NEAR_CHUNK_RADIUS=1`, but FPS stuck near
    `19.8` (3-vsync) because the per-band hardware overhead × 8 bands forms a
    floor that descriptor-count reductions can't break.
  * Root cause: `gpu_transport.c` bumps `hw_sky_epoch` whenever any palette
    index in the generated-sky range (40-63) changes its rgb565 value. The
    sky palette is re-derived every frame from `world_time`, and even
    sub-pixel time-of-day drift flips at least one of the 24 gradient indices
    every frame. So `hw_sky_epoch` advanced every frame, and the
    `hw_sky_band_epoch[buf][band] == hw_sky_epoch` skip check at
    `gpu_transport.c:836-838` was always false.
  * Patch 1 — sky-palette time quantization (`sw/renderer.c`):
    - Added `SKY_PALETTE_TIME_STEP_SECONDS = 0.5f` near the other sky
      constants.
    - In `renderer_draw_sky`, compute `palette_time = floor(time / step) *
      step` and use it for `sun_direction_for_time`, `make_sky_palette`,
      `upload_sky_palette`, and `draw_sky_gradient`. Continuous `time_seconds`
      is still used for cloud azimuth drift and wobble — those don't touch
      the palette so they don't bump the epoch.
    - Effect: `hw_sky_epoch` only advances every 0.5s of world time =
      ~15 frames at 30 FPS. Within those 15 frames every primer-only band
      hits the skip path on whichever physical SDRAM buffer was last
      submitted with the current epoch.
    - 0.5s steps over a 180s day cycle = 360 distinct sky palettes; the
      gradient changes are visually smooth at that granularity.
  * Patch 2 — drop the primer for primer-only cache-miss bands
    (`sw/gpu_transport.c`):
    - In the per-band loop in `gpu_transport_submit_descriptors`, when
      `primer_only` is true and the sky-band-epoch skip check has already
      missed, override the `submit_hw_band_flat` arguments to send `NULL, 0`
      instead of the primer descriptor.
    - The primer existed to absorb a first-pixel pipeline glitch for the
      band's leading raster cycle (palette-pipeline staleness, see the
      header comment around `g_band_primers`). With zero descriptors pushed,
      there is no first pixel to glitch — the RTL `BEGIN_BAND` still fills
      the cache via `VOXEL_EXTMEM_CTRL_SKY_GRADIENT_CLEAR`, no descriptors
      drain through the FIFO, and `END_BAND` flushes the sky-only cache to
      SDRAM. Saves one descriptor's `write_all()` syscall + one descriptor's
      FIFO traversal per primer-only cache miss.
    - Cache bookkeeping is unchanged: `hw_sky_band_epoch[buf][band]` is still
      stamped to `hw_sky_epoch` after the empty submit, so subsequent frames
      hit the full skip path until the epoch advances again.
  * Hardware pre-init during current band flush (RTL):
    - `voxel_gpu.sv` already implements this via cache ping-pong. Lines
      3527-3554 kick `flush_active=1` on `flush_cache_sel = draw_cache_sel`,
      then the next `BEGIN_BAND` toggles `draw_cache_sel` so `ST_CACHE_INIT`
      runs on the OPPOSITE M10K bank. The `band_begin_cache_available` gate
      at lines 964-966 explicitly allows BEGIN to fire while a flush is in
      flight as long as `(~draw_cache_sel) != flush_cache_sel`.
    - `ST_CACHE_INIT` itself is already 2 px/cycle: `cache_maint_addr +=
      16'd2` at line 4268 and dual-port writes at lines 2309-2316. The "two-
      pixel ST_CACHE_INIT failed Quartus fit by 43 LABs" referenced in the
      May 5 plan was a separate, more aggressive variant — see the
      brainstorm section below for ideas on how to land that.
    - No RTL change needed for this item; the existing parallelism is
      already exploiting it.
  * Second A9 core for parallel CPU descriptor work:
    - Cyclone V SoC has dual-core Cortex-A9; we currently run the renderer
      on a single core. Per-frame CPU breakdown after Patch 1+2 is roughly
      `update + begin + draw ~= 6-8ms` of real CPU work, then `end ~= 30-37ms`
      that is mostly blocked on FIFO backpressure (not real CPU work).
    - Proposed pipeline: spawn one worker thread pinned to CPU 1 (via
      `pthread_setaffinity_np`/`sched_setaffinity`). Double-buffer the
      `RenderContext::submit_buffer` and `submit_bytes`. At frame N's
      `renderer_end_frame`, hand the filled buffer to the worker and
      immediately return; main thread then begins frame N+1's draw on
      CPU 0. The worker calls `gpu_transport_submit_descriptors` + flip
      on frame N's buffer, blocking on FIFO backpressure but freeing CPU 0.
      Main thread waits on a "submit done" condvar before starting its
      next `renderer_end_frame`.
    - Net effect: hides the 5-7ms of CPU "draw" behind the 30+ms of HW-
      bound "end". Adds 1 frame of input-to-display latency (acceptable
      for a block game). Frame wall clock goes from
      `(draw=6) + (end=37) = ~43ms` to `max(draw, end) = ~37ms`, putting us
      into the 2-vsync (`33.6ms`) bucket once `end` drops below `33.6ms`
      via Patches 1+2 hits.
    - Synchronization concerns:
      * `transport->hw_sky_epoch` etc. are mutated only inside
        `gpu_transport_submit_descriptors` and palette/fog setters. Worker
        owns submit; main owns palette/fog. Either serialize palette/fog
        through the worker (queue) or hold a mutex around any
        `gpu_transport_set_palette`/`gpu_transport_set_fog` call.
      * `g_bins[]` is module-private to `gpu_transport.c` and only touched
        from inside `submit_descriptors` — naturally single-threaded if
        only the worker calls submit.
      * `g_diag_bbox` / `g_diag_frame_count` / `g_diag_log_flip_next` are
        thread-local-friendly (or guard with mutex).
      * `ctx->submit_buffer` and `ctx->submit_bytes` need two slots and a
        flip pointer; the worker reads slot A while main fills slot B.
    - Implementation footprint estimate: ~150 lines across `renderer.c`,
      `renderer.h`, `game.c`. Risk: pthread plumbing, kernel
      `write()`/`ioctl()` interleaving, debugging timing on hardware.
    - Status: design captured, implementation pending. Worth doing once
      Patches 1+2 are validated on hardware so we know the remaining gap
      we're trying to close.

May 5 — two-pixel `ST_CACHE_INIT` fit brainstorm:

  Context: a previous attempt at a more aggressive 2 px/cycle `ST_CACHE_INIT`
  failed Quartus fit by 43 LABs (resource: `25,997 / 32,070` ALMs already
  consumed, 81% utilization). The current `ST_CACHE_INIT` at lines 4257-4282
  in `voxel_gpu.sv` already advances `cache_maint_addr += 2` and writes both
  even/odd ports with the same `cache_init_rgb565` value, but the failed
  variant presumably did something more — perhaps split the gradient lookup,
  added a wider flat-clear path, or doubled the Z init port count.

  Approaches to try in order of likelihood:

  1. **Replace `ST_CACHE_INIT` with valid-bit clear (May 5 plan item 2).**
     Clear a single 64×640 = 40960-bit valid bitmap (5120 bytes, ~5 M10K
     RAMs at 1 R/W port) instead of writing 40960 pixels of color+Z.
     Reads of invalid pixels return sky color from the gradient ROM and
     `Z=0xFFFF`. Commit on draw sets the valid bit. The bitmap can be
     cleared with a single `memset`-style FSM that writes one M10K row
     per cycle (e.g., 32 bits cleared at once → 1280 cycles for the whole
     band, vs 20480 cycles for full-pixel init).
     - Pros: 16× cycle saving on init alone (~410us → ~25us). Frees ALMs
       since we drop dual-port write logic from `ST_CACHE_INIT`.
     - Cons: adds a second cache port with the gradient ROM read in the
       commit-vs-init mux, plus a valid-bit RAM. May add area in a
       different place than the failed patch did.

  2. **Skip `ST_CACHE_INIT` when the cache already holds the target band's
     sky-cleared template.** Track per-cache `cache_template_band` and
     `cache_template_epoch` registers. After a flush completes, the cache
     still holds the (now-flushed) band's content. If the next BEGIN_BAND
     wants the same band index AND no draw mutated the cache between
     init and flush, skip init entirely.
     - Pros: zero new RAMs; just two small state regs per cache.
     - Cons: needs a `cache_draw_dirty` style flag (already exists at line
       4261), plus careful invalidation when the sky-palette epoch ticks.
       Most useful for static views; overlapping with the SW
       `hw_sky_band_epoch` skip diminishes the win.

  3. **Pipeline the gradient-color computation with the address advance.**
     The current `ST_CACHE_INIT` likely re-reads `cache_init_sky_palette`
     and `palette[]` each cycle; a registered shadow could let two pixels
     per cycle close timing without an extra port. Worth trying only if
     STA shows the gradient-lookup → `fb_wr_data` path is the long path.

  4. **Compress the sky gradient.** Today there are 24 distinct gradient
     palette indices (40-63). If the gradient-band ROM stored
     pre-converted RGB565 directly (with 1-cycle latency for the lookup),
     `ST_CACHE_INIT` no longer needs the full palette LUT path during
     init — it just streams from a tiny 24-entry RGB565 ROM keyed by
     `cache_init_sky_palette`. Reduces routing pressure and palette-port
     contention during init.

  5. **Time-multiplex with `ST_CACHE_FLUSH_COLOR`.** Init and flush
     never run in the same state, so could share a maintenance block
     with internal sequencing. This is a refactor more than an
     optimization, but if the LAB pressure is from duplicated address
     generators, sharing them could save the 43 LABs without changing
     functional behavior.

  Recommended starting point: try (1) — valid-bit clear is the highest
  cycle-saving and is conceptually orthogonal to the previous failed
  variant, so likely lands in different pipeline stages. If it also
  fails fit, try (5) to consolidate maintenance address generators
  before reattempting (1).

May 7 2026: ST_CACHE_INIT Stranded From Banked BRAM Ports
---------------------------------------------------------

Symptoms:

  * VGA output showed the rendered frame replicated 3-4 times vertically
    (entire HUD + sky + clouds + sun stacked as duplicate horizontal
    strips covering the full 480 rows).
  * Sky gradient and HUD rendered correctly inside each strip; world
    blocks (terrain, trees, anything depth-tested) were completely
    missing.
  * The number of replicated copies (3 vs 4) drifted between rebuilds,
    which initially looked like a timing race but was actually a
    function of which stale pixels happened to be left in the cache
    BRAM after the previous frame's ST_DRAW writes.

Diagnosis dead-end (do not repeat):

  * First theory was a 16-bit overflow in `draw_row_base` (`logic
    [15:0]` at `voxel_gpu.sv:199`). On paper, 480*640 = 307200 ≫ 65535,
    and the increments at `draw_row_base <= draw_row_base + 16'd640`
    would wrap. A speculative fix gated the increment on
    `draw_band_index >= cache_band_index` so it only ticks once we
    reach the active band.
  * That fix was a no-op for the actual workload: `gpu_transport.c`
    already bins descriptors per band (fast path at lines 787-799,
    slow path with `clipped_y_min = max(y_min, band_y_min)` at lines
    807-808). For every band-N descriptor, `desc_y_min` is already
    inside the band, so `draw_band_index >= cache_band_index` is
    always true while drawing — the gate never suppressed anything.
    The early-scanline-exit logic also prevents off-band runaway.
  * The `draw_row_base` width was reverted to 16 bits with no gate.

Real root cause:

  * The 2-px-per-cycle refactor split the cache write port into Even
    (`_e`) and Odd (`_o`) lanes. The always_comb block at
    `voxel_gpu.sv:2456` fans the legacy unsuffixed write signals
    (`fb_wr_addr`, `fb_wr_data`, `fb_back_wr_en`, `z_wr_addr`,
    `z_wr_data`, `z_wr_en`) out to the new `_e` / `_o` ports for any
    state that still operates at 1 px/cycle.
  * The exclusion list incorrectly contained `state == ST_CACHE_INIT`:

        if (!(state == ST_DRAW || state == ST_DRAW_FLUSH ||
              state == ST_CACHE_INIT)) begin
            fb_wr_addr_e = fb_wr_addr;
            ...
        end

  * `ST_CACHE_INIT` only drives the unsuffixed signals (`fb_wr_addr =
    cache_maint_addr`, `z_wr_en = 1'b1`, `z_wr_data = 16'hFFFF`,
    etc., at `voxel_gpu.sv:2396-2408`). It does NOT drive the `_e` /
    `_o` ports directly the way `ST_DRAW` / `ST_DRAW_FLUSH` do. So
    once it was excluded from the fan-out block, its writes never
    reached the banked BRAM ports. The cache color and Z words it
    wanted to clear stayed at whatever the previous band's draw had
    left behind.

Why this produced the exact observed image:

  * Missing terrain. With the Z BRAM never reset, depth-tested quads
    (terrain, blocks) tested against stale Z values from the previous
    band's draw — typically near 0 — and lost the depth test
    (`z < z_ref`). Their pixel writes were suppressed.
  * HUD renders. HUD descriptors set `FLAG_ZTEST_BIT = 0` and skip the
    depth test entirely, so they wrote into the color BRAM normally.
  * Vertical replication / ghosting. With the color BRAM never
    cleared, the previous band's writes (often from band 7 — the
    bottom HUD strip) stayed resident. The flush controller then
    copied that stale content out to SDRAM as part of the new band's
    flush, painting the previous band's HUD into the new band's
    SDRAM region. Across 8 bands this manifested as several near-
    duplicate strips of the same content.
  * Perfect sky between strips. When a band's draw produced no
    pixels (e.g., a sky-only band), `cache_draw_dirty` stayed clear
    and the flush controller took the `flush_generated_sky` fast
    path, writing the sky gradient to SDRAM directly without
    touching the broken cache. Those bands looked correct.

Fix:

  * Drop `state == ST_CACHE_INIT` from the exclusion at
    `voxel_gpu.sv:2456-2457`. The block now reads:

        if (!(state == ST_DRAW || state == ST_DRAW_FLUSH)) begin

  * `ST_DRAW` / `ST_DRAW_FLUSH` remain excluded because they drive
    `_e` / `_o` directly inside the case (lines 2426-2451) using
    `commit_addr` / `commit_addr_o`, and the post-case fan-out would
    clobber that. Every other state — including `ST_CACHE_INIT`,
    `ST_CLEAR`, `ST_CACHE_LOAD_COLOR`, `ST_CACHE_LOAD_Z`, sky-patch
    `ST_FETCH` — only drives the unsuffixed signals and depends on
    this block to reach the BRAM ports.

Verification:

  * On hardware after rebuild + reflash: terrain renders, HUD
    renders, and the vertical replication is gone in a single pass.

Lesson / how to avoid repeating this:

  * Whenever a state writes the cache BRAM, it must either drive the
    `_e` / `_o` ports directly inside its case branch (the
    `ST_DRAW` pattern) or fall through the fan-out block at
    `voxel_gpu.sv:2456`. If a new state is added to the exclusion
    list, audit that the state assigns `fb_wr_addr_e`, `fb_wr_addr_o`,
    `fb_back_wr_en_e`, `fb_back_wr_en_o`, and the matching Z lane
    signals itself.
  * "Symptom looks like an addressing wrap" is a misleading prior.
    A stale-cache-write bug can produce identical-looking vertical
    replication because the flush controller is faithfully copying
    whatever the cache happens to contain, including unwanted leftover
    pixels from a different band.

May 7 2026: Hardware Per-State Cycle Counters
---------------------------------------------

Motivation:

  * The pre-existing `init=`, `flush_raw=`, `flush_slack=` fields in the
    `renderer: calc` log line are SOFTWARE COST ESTIMATES generated by
    `gpu_transport.c:290` (`cost->init_cycles[band] += pixels` etc.),
    not measurements of what the RTL actually does. Optimization
    decisions based on those numbers were drifting away from real
    hardware behavior.
  * To resolve which band-state was actually consuming the band wall-
    clock, six free-running 32-bit counters were added to the RTL,
    exposed over MMIO, drained via ioctl on every FLIP, and printed
    by the renderer alongside the existing `calc` line.

Counters added (`hw/voxel_gpu/rtl/voxel_gpu.sv:167-183`):

  * `perf_draw_active`  — cycles in `ST_DRAW`/`ST_DRAW_FLUSH` while a
    pixel commit happened (`commit_valid || commit_valid_o`).
  * `perf_draw_idle`    — cycles in `ST_DRAW`/`ST_DRAW_FLUSH` with no
    commit (rasterizer starved on descriptors).
  * `perf_flush_active` — cycles where the bg flush pushed a word
    into the WR FIFO (`bg_flush_wr_push || main_flush_wr_push`).
  * `perf_flush_stall`  — cycles where bg flush was alive but did not
    push (FIFO push gated by `scanout_write_slack` pre-Tier-1, or
    backpressure post-Tier-1).
  * `perf_init`         — cycles in `ST_CACHE_INIT`.
  * `perf_load`         — cycles in
    `ST_CACHE_LOAD_*`/`ST_CACHE_DRAIN_*`/`ST_CACHE_START_LOAD_Z`.

  All six reset on the FLIP write to `ADDR_CONTROL` (writedata[1] =
  CMD_FLIP), so the read on the *next* FLIP returns counts for the
  whole frame just submitted. The reset gate is `reset || perf_flip_write`
  inside the dedicated `always_ff` block at `voxel_gpu.sv:4575-4608` —
  kept separate from the main FSM `always_ff` reset to avoid multi-driver.

MMIO + driver plumbing:

  * RTL exposes counters at register offsets `13'h010..13'h015`
    (byte addresses `0x0040..0x0054`) via the readdata case at
    `voxel_gpu.sv:4622-4628`.
  * Header (`sw/voxel_gpu.h`): `VOXEL_REG_PERF_*` defines, struct
    `voxel_perf_counters` (six `__u32`), and ioctl
    `VOXEL_IOC_GET_PERF` (cmd 13). `VOXEL_IOC_MAXNR` bumped to 13.
  * Kernel module (`sw/voxel_gpu.c`): handler `voxel_ioc_get_perf`
    reads all six MMIO registers under `voxdev.lock`. Switch case
    added.
  * Renderer (`sw/gpu_transport.c`, just before the FLIP submission
    at line ~1033): `ioctl(VOXEL_IOC_GET_PERF)` then `fprintf` of
    the new `renderer: hw_perf` line with all six values converted
    to ms (50 MHz clock = 50000 cycles/ms) plus `draw_busy` /
    `flush_busy` percentages.

How to interpret the line:

  ```
  renderer: hw_perf draw_act=X.XXms draw_idle=X.XXms
                    flush_act=X.XXms flush_stall=X.XXms
                    init=X.XXms load=X.XXms
                    (draw_busy=NN% flush_busy=NN%)
  ```

  * `flush_busy` < 30%: SDRAM arbitration is throttling background
    flush. Look at Tier 1 / Tier 2 of the May-7 arbitration plan.
  * `draw_idle` > 5%: rasterizer is starved (descriptor upload, FETCH
    stalls, or main FSM not feeding bands fast enough).
  * `init` ≈ `flush_active`: ST_CACHE_INIT is fully hidden by the
    flush ping-pong. Eliminating ST_CACHE_INIT (e.g. May-5 plan
    item #1, valid-bit clear) would NOT save wall-clock — it runs
    in parallel with flush.
  * `load` > 0: cache reuse is failing — `cache_band_valid` logic
    is dropping a hit. Investigate band invalidation.
  * Sums can exceed wall-clock: `init` and `flush_active` overlap
    via the ping-pong; `draw_active` and `flush_active` can also
    overlap when the rasterizer drains while the prior band flushes.

Initial findings (pre-Tier-1):

  * Across moving and stationary frames: `flush_act ≈ 6 ms`,
    `flush_stall ≈ 25 ms`, `flush_busy ≈ 17–22 %`. The bg flush
    spent ~80% of its alive time blocked on `scanout_write_slack`.
  * `draw_idle ≈ 0.5 ms`, `draw_busy ≈ 85–98 %`. Rasterizer was
    not the bottleneck.
  * `init ≈ flush_act ≈ 6 ms` confirmed ping-pong overlap.
  * `load = 0 ms`: full-band reuse was working steady-state.

  Conclusion: the band wall-clock was set by SDRAM contention with
  scanout, not by any compute path. The May-5 plan item #1 (replace
  ST_CACHE_INIT with valid-bit clear) was correctly skipped because
  the counter data showed init runs entirely in parallel with flush.

May 7 2026: Tier 1 SDRAM Arbitration Decoupling
-----------------------------------------------

Motivation:

  * Per the new hw_perf counters, bg flush was alive ~31 ms per band
    but only pushed words ~6 ms (~17–22 % busy). The remaining
    ~25 ms was `flush_stall` — gated off by `scanout_write_slack`.
  * The original gate served three different roles in
    `voxel_gpu.sv`:
      1. cache→WR-FIFO push (`bg_flush_wr_push`, line 1133)
      2. flush controller's next cache read
         (`flush_can_issue_read`/`flush_can_issue_sky`,
         lines 1173–1189)
      3. SDRAM controller `WR_LENGTH` (`sdram_wr_length_cfg`,
         line 1191) — the actual external-bus arbiter
  * Only (3) actually contends with scanout reads on the external
    SDRAM pins. (1) and (2) are entirely internal: pushing into
    the controller's WR FIFO is M10K-only and uses zero external
    bus cycles until `WR_LENGTH > 0` triggers a burst. Gating all
    three on the same signal kept flush idle through every scanout
    read window even when its internal pipeline had free capacity.
  * `COPY_WR_FIFO_HIGH_WATER = 64` (line 88) compounded the
    throttle: even when (1) and (2) cleared, flush stopped issuing
    cache reads after 64 words staged, far below the actual
    `Sdram_WR_FIFO` depth of 512 words
    (`sdram_local_test/Sdram_WR_FIFO.v:101`,
    `dcfifo_component.lpm_numwords = 512`).

RTL changes (all in `hw/voxel_gpu/rtl/voxel_gpu.sv`):

  * `COPY_WR_FIFO_HIGH_WATER`: `9'd64` → `9'd224`. Leaves 288 words
    of headroom in the 512-deep FIFO for in-flight pushes after
    `!sdram_wr_full` deasserts.
  * `bg_flush_wr_push`: dropped `&& scanout_write_slack`. Hard
    backpressure preserved by `!sdram_wr_full`.
  * `flush_can_issue_read`: dropped `scanout_write_slack &&`.
    `sdram_wr_use < COPY_WR_FIFO_HIGH_WATER` is the new
    backpressure gate.
  * `flush_can_issue_sky`: same as above.

Deliberately NOT changed:

  * `main_flush_wr_push` (line 1141) and `cache_can_issue_read`
    (line 1163) — main-FSM cache-flush path retains the full gate.
    Decoupling only applied to the background flush controller to
    contain blast radius.
  * `sdram_wr_length_cfg` (line 1191) — the actual external-bus
    arbiter is untouched. Nothing reaches SDRAM during scanout
    reads; the FIFO just pre-stages.
  * `COPY_DRAIN_CYCLES` (line 92, value `8'd128`) — Gemini's red-
    team review flagged a risk that deeper pre-staging could let
    the buffer flip race with words still in the FIFO. Re-reading
    `voxel_gpu.sv:3380-3397` shows the drain timer only starts
    counting once `sdram_wr_use[8:0] == 9'd0`, so it is a post-
    empty controller-pipeline settling timer, not a wait-for-FIFO
    timer. The FIFO-empty gate is what protects the flip; the
    timer just covers internal column-write latency. 128 cycles
    (2.56 µs) at 50 MHz is ~8× a typical SDR burst+precharge — no
    bump needed.

Gemini's red-team feedback (rebutted points captured here for the
record):

  1. WR FIFO push is electrically free of external bus cost (Altera
     Sdram_Control front-end FIFO is on-chip M10K/MLAB; pushes do
     not nibble scanout bandwidth). ✓ confirmed.
  2. No hidden ordering requirement between WR_LENGTH and WR_LOAD —
     IP latches at WR_LOAD pulse; staging earlier is the preferred
     use case. ✓ confirmed.
  3. 224/256 high-water margin claim — actual FIFO is 512 deep, so
     the margin is even more generous than the review assumed.
  4. COPY_DRAIN_CYCLES bump claim — based on a misread of where
     the timer counts; see RTL comment above. NOT applied.
  5. `band_flush_pending` gating — not needed, back-buffer
     guarantee already enforced. ✓ confirmed.

Verification (post-Tier-1 hw_perf data, moving + stationary mixed
camera):

  * `flush_act`: 6 ms → 6.2 ms (essentially unchanged; we already
    pushed at full bandwidth during the windows we got).
  * `flush_stall`: 25 ms → ~10.5 ms (≈14 ms reduction).
  * `flush_busy`: 17–22 % → ~37 %. ~2× more useful flush time per
    band.
  * `draw_busy`: 85–95 % unchanged. No regression in rasterizer
    feed.
  * `load`: still 0 ms. Cache reuse path not perturbed.
  * No visual artefacts (no top-of-frame tearing, no scanout
    starvation symptoms).
  * Steady-state FPS now sits at 29.7–29.9. The remaining drops to
    23–24 FPS correlate with `update=8–10 ms` spikes, not GPU work
    (see "Next bottleneck" below).

Next bottleneck: CPU-side `update` spikes
-----------------------------------------

After Tier 1, the GPU side reliably fits inside the 33.6 ms 30-FPS
budget. FPS dips to 22–24 still occur in the captured run, with a
clear and consistent signature:

  * Good frames: `update ≈ 0.02–0.04 ms`.
  * Bad frames: `update ≈ 8.7–10.3 ms`.
  * Bad frames cluster on player positions that just crossed a
    chunk boundary (e.g. (0.0,…) → (3.0,…), (-2.0,…) → (0.7,…),
    (15.6,…) → (17.7,…)).
  * `max_work` occasionally spikes to 220–270 ms — single-frame
    chunk-generation bursts.

`update` is measured in `sw/game.c:635` as the time between
`loop_start` and `render_start`. It includes `input_update`,
`chat_tick`, the physics-step `while` loop, `world_stream_around`
(`game.c:578`), and `world_rebuild_lighting` /
`world_rebuild_dirty_meshes` (`game.c:606-613`). The chunk-cross
correlation points squarely at the streaming and mesh-build pair.

Candidate fixes (ranked by expected ROI vs effort):

  1. Cap mesh rebuilds per frame in `world_rebuild_dirty_meshes`
     (`sw/world.c:1558`). Spread the cost over multiple frames;
     accept brief pop-in at chunk boundaries. Smallest change,
     biggest immediate win.
  2. Move chunk generation + mesh build to a worker thread.
     Largest win — fully hides the cost — but requires double-
     buffered chunk storage and proper sync between the streamer
     thread and `renderer_draw_world`.
  3. Wider streaming border so chunks are pre-generated earlier
     in the player's path. Reduces the size of the boundary spike
     but does not eliminate it; cheap to try.
  4. Greedy-meshing or per-band frustum culling — would help
     `bands` time when quad count is high but does not address
     the dominant `update` spike. Defer.

Tier 2 / Tier 3 of the SDRAM arbitration plan
(`ACTIVE_WRITE_END_HCOUNT` bump and 4th scanout line buffer) are
likely unnecessary now that the GPU consistently meets budget;
hold them in reserve and revisit only if `flush_busy` becomes
load-bearing again after the CPU-side fixes land.

May 7 2026: Phase A async mesh rebuilds
---------------------------------------

Goal:

  * Remove chunk meshing from the foreground `update` path without
    making the renderer take a coarse world lock.
  * Keep initial world generation synchronous so startup still has a
    complete visible mesh before the first frame.

Implementation:

  * `ChunkMesh` is now an immutable published snapshot:
    `chunk->live_mesh` is atomically loaded by `renderer_draw_world`,
    while `chunk->faces` remains rebuild scratch owned by `world.c`.
  * `rebuild_chunk_faces()` publishes a new `ChunkMesh` with atomic
    exchange. The previous live mesh moves to `retired_mesh`.
  * `mesh_worker.c` owns one background pthread and a bounded SPSC job
    queue of `(chunk_x, chunk_z, generation)` jobs. Jobs use coordinates
    rather than `Chunk *` because the chunk array can be compacted by
    streaming.
  * `CHUNK_FLAG_MESH_QUEUED` prevents duplicate jobs for one dirty
    chunk. The dirty bit stays set while queued and is cleared only
    when the worker successfully publishes the mesh.
  * `world->world_mu` protects chunk-array/block/light mutation from
    worker rebuild reads. The renderer does not take this mutex; it
    reads the stable `live_mesh` pointer captured at the start of
    `renderer_draw_world`.
  * When `mesh_worker_start()` succeeds, it enables
    `world->async_mesh_rebuilds_enabled`. From that point,
    `world_stream_around()` marks dirty chunks and returns without
    synchronously rebuilding meshes. The game loop calls
    `mesh_worker_drain_dirty()` before render and
    `mesh_worker_reap_retired()` after `renderer_end_frame()`.
  * Set `VOXEL_MESH_WORKER=0` to keep the old synchronous fallback.

Scope / remaining bottlenecks:

  * Phase A moves mesh rebuilds off the main frame, but chunk terrain
    generation, snapshot load/save, and full lighting rebuilds still
    happen synchronously inside `world_stream_around()`.
  * If hardware still shows `update` spikes after this, the next target
    is chunk generation / persistence streaming, not the renderer.

Follow-up from first hardware run:

  * Hardware logs with `mesh_worker: on` still showed chunk-boundary
    dips, but the `update` averages had fallen into the ~3-5 ms range
    while `max_work` stayed high. That pointed away from synchronous
    meshing and back toward streaming work still performed on the main
    thread.
  * The streamer was still calling a full-world lighting rebuild after
    every window shift. In worlds with no loaded light emitters this is
    unnecessary: sky light is per-column/per-chunk and terrain
    generation contains no lamps.
  * The no-emitter path now rebuilds sky light only for newly loaded
    chunks, skips the global lighting rebuild, and avoids dirtying every
    loaded chunk. A maintained `world->has_light_emitters` flag keeps
    the common no-lamps case from scanning the whole world on every
    boundary crossing. If a lamp exists or one was just evicted, the old
    full rebuild path is preserved to keep block-light propagation
    correct.

Verification:

  * `make tests/world_chunk_test` and `./tests/world_chunk_test` pass.
  * Renderer test targets compile with the new atomic mesh path.
  * A local worker smoke test starts the worker, edits a block, drains
    dirty mesh work, stops the worker, and exits cleanly.

May 7 2026: Hardware Headroom Tier 2 / Tier 3 Patch Plan
--------------------------------------------------------

Why revisit hardware:

  * After async meshing and the no-emitter streaming-lighting fast path,
    chunk-boundary dips are smaller but still visible.
  * The GPU side is not the dominant root cause of those dips, but
    `hw_perf` still commonly reports `flush_act ~= 5.4ms` and
    `flush_stall ~= 9.1-9.6ms` (`flush_busy ~= 36-38%`). Reducing that
    stall gives the software update path more headroom before it misses
    the 33.6ms 30-FPS bucket.

Tier 2 change:

  * Increase `ACTIVE_WRITE_END_HCOUNT` from `960` to `1120`.
  * This lets the SDRAM write side continue launching background flush
    bursts deeper into the visible scanline. It still leaves the last
    160 50-MHz cycles of active video plus the horizontal blank interval
    for scanout recovery before the next line.
  * This is intentionally cautious: `1200+` might be possible, but the
    first hardware pass should measure scanout safety and `flush_stall`
    before pushing closer to the visible-line edge.

Tier 3 attempt / fitter follow-up:

  * First attempt: add a fourth scanout line buffer/bank so the scheduler
    can protect current, target, immediate-next, and far-next rows at the
    same time.
  * Board build rejected that version: Quartus reported `3790` LABs
    required for a `3207`-LAB device. The scanline buffers are async-read
    MLAB/LUT-memory structures, so the extra bank spent scarce LAB fabric
    instead of cheap block RAM.
  * Backed out the fourth physical bank and kept the three-buffer design.
  * Replacement low-LAB Tier 3 change: split scanout prefetch into
    critical rows (current, target, immediate-next) and best-effort
    far-next. Far-next prefetch now backs off while cache flush / SDRAM
    write pressure exists; critical prefetch still blocks writes.

Expected result:

  * Lower `flush_stall` and slightly shorter `renderer_end_frame()` on
    block-heavy views, without increasing LAB pressure.
  * No direct fix for `update=max_work` chunk-generation spikes; this
    only buys frame-budget headroom while software streaming work is
    still being reduced.

May 7 2026: Runtime Streaming / Mesh Settings
---------------------------------------------

Motivation:

  * Hardware headroom helped only a little. The remaining bad frames still
    correlate with chunk crossings: `update` spikes into the 4-14 ms range
    and `max_work` can jump well over 100 ms.
  * The main-thread streamer still generated/loaded a whole new border
    immediately when the loaded window shifted. With a 1-chunk hidden
    border, those chunks usually do not need to appear in the same frame.

Changes:

  * Added `world->stream_chunks_per_frame`.
    - `0` means unlimited / old behavior.
    - Positive values cap new generated/loaded chunks per
      `world_stream_around()` call after the initial full world load.
    - Missing chunks are filled nearest-to-center first over subsequent
      frames.
    - Environment: `VOXEL_CHUNKS_PER_FRAME` (also accepts the singular
      `VOXEL_CHUNK_PER_FRAME`). The game default is `2`.
  * Added `world->near_chunk_radius` to replace the compile-time
    `NEAR_CHUNK_RADIUS`. Lower values apply greedy meshing to more visible
    chunks, trading possible close-up T-junction shimmer for fewer
    descriptors.
    - Environment: `VOXEL_NEAR_CHUNK_RADIUS`. Default is `1` (matches the
      prior compile-time value), clamped to
      `[0, render_distance_chunks]`. `0` means every visible chunk uses
      greedy meshing.
    - Changing it at runtime marks near/far transition chunks mesh-dirty;
      the mesh worker rebuilds them asynchronously.
  * The ESC pause overlay now has a small settings panel:
    - `STREAM CHUNKS/FRAME`
    - `NEAR MESH RADIUS`
    - `W/S` selects a row, `A/D` adjusts.

World storage note:

  * Current persistence stores only modified chunk snapshots over the
    deterministic procedural world.
  * A future pregenerated-world mode should promote chunk snapshots to a
    first-class world database/cache so terrain generation and snapshot
    load can move fully off the foreground frame path.

May 7 2026: Async Chunk Generation (Minecraft-style)
----------------------------------------------------

Motivation:

  * The streaming cap helped, but the underlying generation work
    (terrain noise, snapshot disk I/O, sky-light rebuild) still ran on
    the main thread when slots were allocated. Spreading it over frames
    only delayed the spikes.
  * We already match Minecraft's data model: deterministic seed +
    snapshot deltas. The missing piece was running gen on a worker
    thread, the way Minecraft does, so chunk-boundary crossings never
    block the render thread.

Changes:

  * New `CHUNK_FLAG_LOADING`: a chunk slot can be present (allocated +
    findable via `chunk_lookup`) before its blocks/lighting are filled.
    `world_get_block` + the mesh worker treat LOADING-only slots as
    AIR. Slot-presence checks (`chunk_slot_is_present`) use
    `LOADED | LOADING` so streaming/eviction handles both.
  * New `CHUNK_FLAG_GEN_QUEUED`: marks a LOADING chunk that is already
    in the gen worker queue, so `gen_worker_drain_pending` does not
    double-enqueue.
  * New file pair `gen_worker.{c,h}`: SPSC ring buffer + dedicated
    pthread, mirrored on `mesh_worker.c`. Job is
    `(chunk_x, chunk_z, generation)`. Worker:
      1. Pops the job.
      2. Calls `world_async_chunk_gen_offline` which heap-allocates a
         scratch `Chunk`, runs `generate_chunk_terrain`,
         `load_chunk_snapshot`, `rebuild_chunk_sky_lighting`, and
         copies blocks/sky-light/has-emitters into a `ChunkGenResult`.
         No locks held during this work - it only reads
         `procedural_seed`, `stone_tries_per_chunk`,
         `persistence_enabled`, and `save_root`, all immutable
         post-init.
      3. Calls `world_finalize_async_chunk_load` which takes
         `world_mu` briefly, finds the slot by coords, checks the
         generation matches, copies the buffer in, flips
         `LOADING -> LOADED | MESH_DIRTY`, and dirties the four
         neighbors so the mesh worker rebuilds them.
  * `stream_generate_chunk` now branches on
    `world->async_chunk_gen_enabled`. Async path allocates a LOADING
    slot, inserts into the lookup, and returns immediately - the main
    thread never runs gen on a chunk-cross. The synchronous path is
    kept as a fallback for `VOXEL_GEN_WORKER=0` and for the very first
    streaming call inside `world_init_infinite_procedural`, which runs
    before the gen worker is started so the player spawns on solid
    ground.
  * `retain_chunks_in_window` now keeps both LOADED and LOADING slots,
    so a chunk-window shift while a gen job is in flight doesn't
    silently drop the slot.
  * Stale jobs (chunk evicted then re-streamed before the worker ran,
    new generation) are detected on the finalize path and dropped -
    same pattern as the mesh worker.
  * `gen_worker_drain_pending` is called once per frame in `game.c`
    before `mesh_worker_drain_dirty`, so finalized chunks land in the
    mesh queue the same frame.

Lock discipline:

  * Generation work runs lock-free on the worker.
  * `world_finalize_async_chunk_load` is the only point where the
    worker takes `world_mu`, and it does so briefly (one
    `chunk_lookup_find_index` + two `memcpy`s the size of one chunk
    buffer).
  * Drain takes `world_mu` first, then `queue_mu` - the worker never
    holds both, so no deadlock.

Trade-offs / known v1 limits:

  * Block-light BFS still rebuilds globally on next stream when a
    finalized chunk has emitters. For default-procedural (no emitters)
    this is a no-op; player-placed lamps trigger an existing
    synchronous path. Acceptable until torch-heavy worlds become a
    problem.
  * On-thread mesh rebuild can run before all neighboring LOADING
    chunks have finalized, so a chunk's mesh may be built up to twice
    (once with neighbors-as-AIR, once after each neighbor finalizes).
    Bounded by `MESH_QUEUED` so the cost is at most a handful of
    extra rebuilds per chunk crossing.
  * If `world_async_chunk_gen_offline` fails (snapshot I/O error /
    OOM), the slot stays LOADING|GEN_QUEUED until the player walks
    away and `retain_chunks_in_window` evicts it. Logged via stderr.

Env / runtime knobs:

  * `VOXEL_GEN_WORKER=0` disables the worker (start returns false,
    streaming runs synchronously - same code path as before this
    patch).

May 7 2026: Mesh Snapshot Regression Fix
----------------------------------------

Context:

  * Commit `b749cfd` tried to reduce mesh-worker lock time by copying only
    the one neighbor border slice needed for face exposure. Hardware logs
    showed no useful FPS improvement, and playtesting showed visual
    aberrations.

Fix:

  * Rolled the mesh-worker snapshot back to full self + cardinal-neighbor
    chunk copies. The risky border-slice optimization is not worth keeping
    without a clear frame-time win.
  * Removed the trylock/requeue behavior from the mesh worker. It conflated
    "world lock busy, retry later" with "stale/evicted job, drop it", which
    could keep requeueing dead jobs.
  * Restored `VOXEL_GEN_PUSHES_PER_FRAME` as an optional override on top of
    the runtime stream chunk cap.
  * Added a regression in `world_chunk_test`: a worker rebuild of a chunk
    whose z-boundary face is occluded by a neighboring chunk must not expose
    that hidden face. This catches future partial-copy attempts that miss
    `BlockID` element sizing or border orientation.

May 7 2026: Affinity + Update Spike Guardrails
----------------------------------------------

Why:

  * After async chunk generation, the hardest FPS drops still show up as
    `update` spikes. Some frames report many physics catch-up steps after a
    stall, which turns one hitch into a visible spiral.
  * DE1-SoC has two Cortex-A9 cores, so pinning the foreground thread away
    from generation/mesh workers is worth testing.

Changes:

  * Added Linux-only CPU affinity helpers:
    - `VOXEL_PIN_THREADS=1` enables pinning. It defaults off after field
      testing showed forced pinning could make boundary-crossing FPS worse
      on the DE1-SoC image.
    - `VOXEL_MAIN_CPU` defaults to `0`.
    - `VOXEL_MESH_CPU` defaults to `1`.
    - `VOXEL_GEN_CPU` defaults to `1`.
    - `VOXEL_AFFINITY_LOG=1` logs decisions; `DEBUG=1` also logs.
  * Added a physics catch-up cap:
    - `VOXEL_MAX_PHYSICS_STEPS_PER_FRAME`, default `4`.
    - `0` disables the cap.
    - Dropped catch-up steps are reported as `drop=` in the perf line.
  * Expanded perf diagnostics with update breakdown fields:
    - `upd_phys`
    - `stream`
    - `light`
    - `gen`
    - `mesh`

May 7 2026: Faster Stream Window Retention
------------------------------------------

Finding:

  * The granular perf log showed the async-era FPS drops were dominated by
    `stream=...`, often tens of milliseconds. Lighting, gen-drain, and
    mesh-drain were usually near zero.
  * `retain_chunks_in_window()` compacted the active chunk array by copying
    almost every retained `Chunk` after the first eviction. A `Chunk` owns
    block arrays, light arrays, mesh atomics, and face scratch pointers, so
    this was a large foreground memcpy on every chunk-boundary crossing.

Change:

  * Retention now fills holes from the right edge of the active range,
    copying at most roughly the number of evicted slots instead of all
    retained slots after the first hole. Chunk order is no longer stable,
    but all readers already go through `chunk_lookup`, which is rebuilt
    after retention.
  * Async placeholder allocation no longer clears block/sky arrays that are
    invisible while `CHUNK_FLAG_LOADING` is set. `block_light` is cleared at
    finalize because async results currently copy blocks + sky only.

Follow-up:

  * Field logs still showed boundary hitches with `stream=...` dominating
    while `gen` and `mesh` stayed small. That means the cliff is still in the
    foreground streaming path, not in terrain generation itself.
  * `stream_world_to_chunk_center()` now treats "same center/window but not
    full yet" as a continuation fill. It skips retention, persistence scans,
    lookup rebuilds, perimeter dirty marking, and stream-epoch churn while
    gradually adding the remaining async placeholders.
  * Mesh worker queueing is now paced by `VOXEL_MESH_PUSHES_PER_FRAME`; unset
    mirrors `VOXEL_CHUNKS_PER_FRAME`, and `0` means unlimited. The default is
    now `1`, which prevents a single boundary crossing from dumping a large
    mesh backlog onto the worker.
  * Perf logs now split `stream` into `wait` and `body`. `wait` is time spent
    waiting for `world_mu`; `body` is actual stream work. Mesh/gen workers yield
    before taking `world_mu` while a foreground stream lock is pending.
  * Startup/full-window streaming now ignores `VOXEL_MESH_REBUILDS_PER_FRAME`
    for the initial synchronous mesh build. This avoids booting into a world
    with only one chunk mesh when an old diagnostic env var is still set.

Prefetch direction:

  * The cleaner long-term fix is a staging cache: predict the player's movement
    direction, slowly generate/load the next edge outside the active 9x9 window,
    then promote already-ready chunks when the center crosses a boundary. That
    turns boundary crossing into pointer/index promotion instead of generation.
    This is larger than the current smoothing patch because staged chunks need
    separate lifetime, lookup, and persistence rules.

Lighting direction:

  * Streaming no longer performs non-initial full-window block-light rebuilds
    inline. If loaded/generated chunks require a relight, streaming marks
    `lighting_dirty`; the game loop rebuilds it only after chunk streaming is
    quiet and the player is not moving horizontally. This keeps saved lamps
    from turning boundary crossings into hidden `stream body` spikes.
  * A better model is an incremental light job queue:
    - Sky light stays chunk-local and is already rebuilt per generated chunk.
    - Persist/load `block_light` with chunk snapshots or rebuild only the
      newly loaded chunk plus a one-chunk neighbor shell.
    - For block edits or streamed-in emitters, enqueue add/remove BFS work
      from affected emitters and cap light nodes per frame.
    - Render old light until the queued light update finishes, same as old
      meshes stay visible until a worker publishes a new mesh.

May 7 2026: Three Latency Trims (Light Diff, Late Mouse, Edit Priority)
-----------------------------------------------------------------------

Context:

  * After deferred lighting landed, perf logs showed solid 30 FPS with
    `mesh=1.7ms` for ~17 frames after each `light=2.7-3.6ms` rebuild. Three
    distinct opportunities remained: a post-light mesh wave that re-meshed
    chunks whose lighting did not actually change, a 5-15 ms input-to-display
    gap on heavy frames, and slow click response when the mesh queue carried
    a backlog.

Light-rebuild diff:

  * `world_rebuild_lighting_locked()` previously ended with
    `mark_all_loaded_chunks_mesh_dirty()`. On a stable world (no edits, no
    new chunks, lights unchanged) the rebuild produces identical light
    arrays, but the worker still chewed through the entire 9x9 window.
  * It now snapshots `sky_light` and `block_light` per loaded chunk before
    `clear_world_lighting()`, clears `MESH_DIRTY` (so BFS-time
    `mark_chunk_and_adjacent_dirty_for_block` calls do not leak through),
    then post-rebuild memcmps against the snapshot. A chunk re-enters dirty
    only if its own lighting changed, its prior dirty state was set, or any
    cardinal neighbor's lighting changed (face shading samples across).
  * On allocation failure it falls back to the old "mark all" path.
  * Net: an idle relight is now zero post-rebuild mesh work.

Late mouse re-sample:

  * `game.c` now calls `input_update()` a second time right before
    `renderer_set_camera()` and folds the accumulated mouse delta into the
    camera there. The early apply at the top of the loop still feeds physics
    direction, which is what `wish_x/wish_z` depend on.
  * Edge events (jump, break, place) the late poll picks up are not
    consumed in the late phase - they roll into the next frame's normal
    handling, so we never lose a press.
  * Closes the input-to-display gap from "frame top" to "render start"
    (typically 5-15 ms on the FPGA target on busy frames).

Mesh worker priority lane:

  * Added a small (32-slot) priority FIFO to `mesh_worker.c` alongside the
    existing 1024-slot main queue. The worker pops priority first, falls
    back to main queue.
  * New `CHUNK_FLAG_MESH_EDIT_PRIORITY` flag and
    `world_mark_chunk_mesh_edit_priority(world, wx, wz)` API.
    `try_break_targeted_block` and `try_place_targeted_block` set the flag
    on the edited chunk after `world_set_block` returns true.
  * `mesh_worker_drain_dirty` now branches on the flag: priority chunks
    bypass the per-frame push cap and route to the priority queue. If a
    chunk is already in the main queue and the priority flag is set, drain
    pushes a fresh job (current generation) onto the priority queue; the
    older main-queue copy is discarded by `world_run_mesh_job`'s generation
    check when it eventually pops.
  * Priority queue full -> leave `MESH_EDIT_PRIORITY` set, retry next
    frame; no fallback to main queue (we want head-of-line semantics).
  * Net: click-to-mesh stays sub-frame even when the main queue carries a
    streaming backlog.

Files touched:

  * `sw/world.c`, `sw/world.h` - light-rebuild diff, new `MESH_EDIT_PRIORITY`
    flag, `world_mark_chunk_mesh_edit_priority` implementation.
  * `sw/mesh_worker.c` - priority queue ring, dual-pop worker logic, drain
    routing.
  * `sw/game.c` - late mouse re-sample before `renderer_set_camera`,
    priority-flag calls in the break/place handlers.


May 7 2026: Bin-During-Emit (Eliminate Second-Pass Descriptor Walk)
===================================================================

Context: Post-tier-1 + async mesh + edit-priority shipped. Perf showed
`draw_busy=92-97%` (FPGA-bound), but ~0.3-1 ms/frame was still spent in
the second-pass `submit_hw_binned` walk that re-reads the whole
descriptor stream from `submit_buffer` to bin into per-band buffers.

Optimization: front-load the per-band routing. The renderer already
knows each descriptor's `y_min/y_max` at emit time, so do the binning
right after writing the descriptor (still hot in L1) instead of as a
separate pass.

Mechanism:

  * Pulled the per-quad logic out of `submit_hw_binned`'s inner loop
    into `bin_one_descriptor(transport, desc, size, diag*)` -- the
    redundant-sky-clear filter, the off-screen y-clip skip, the
    single-band fast path (memcpy-only), and the multi-band slow path
    (rewrite_clipped per band).
  * Two new public APIs in `gpu_transport.h`:
      - `gpu_transport_begin_descriptors(transport)` resets the current
        per-band bin set, appends the per-band primer descriptors, zeros
        the staging diag accumulators, and sets
        `transport->staged_active = 1`. HW-mode only (no-op for pure
        socket).
      - `gpu_transport_bin_descriptor(transport, desc, size)` calls
        `bin_one_descriptor` if `staged_active` is set; otherwise no-op.
  * `submit_hw_binned`: when `staged_active` is set, copy staged diag
    state into locals, clear the flag, `goto skip_binning`. Legacy path
    (no `begin_descriptors` caller, e.g. socket-only) unchanged --
    re-runs the binning loop from the contiguous stream.
  * `renderer_begin_frame`: now calls `gpu_transport_begin_descriptors`.
  * `stage_prepared_quad`: after committing `submit_bytes` and
    `n_quads`, calls `gpu_transport_bin_descriptor(ctx->transport, d,
    emitted_size)`.

Why the contiguous `submit_buffer` is still maintained: the socket
backend (virtual_gpu over UNIX socket) consumes the contiguous stream
verbatim. Keeping it in parallel costs the descriptor's struct-field
stores (already paid on the HW path too -- they are direct writes, not
memcpy), so the socket backend keeps working without touching its
codepath.

writev coalescing was investigated and **rejected**. The kernel driver
(`voxel_gpu.c`) only registers `.write`, not `.write_iter`. Per-band
framing is `ioctl(BEGIN_BAND)` -> `write` -> `ioctl(END_BAND)`, with the
ioctl between bands required to switch the on-chip band cache index.
We already coalesce all of a band's descriptors into one `write_all`
call, so writev saves zero syscalls without driver changes.

Files touched:

  * `sw/gpu_transport.h` -- two new API decls.
  * `sw/gpu_transport.c` -- `bin_one_descriptor` helper,
    `reset_bins_and_prime` helper, public begin/bin functions, staged
    diag globals, staged-mode short-circuit in `submit_hw_binned`.
  * `sw/renderer.c` -- begin call in `renderer_begin_frame`, bin call
    at the tail of `stage_prepared_quad`.

Expected impact: ~0.3-1 ms/frame in HW mode. Not a huge win on its
own, but cleans up the path for the bigger upcoming optimization
(frame pipelining: overlap CPU prep of frame N+1 with FPGA flush of
frame N).


May 7 2026: Opt-In Frame Pipelining (CPU Emit While FPGA Drains)
================================================================

Context: after bin-during-emit, the remaining wall time is mostly
serialized FPGA work inside `renderer_end_frame`: `CLEAR`, 8x
`BEGIN_BAND/write/END_BAND`, then `FLIP`. Perf logs showed frames where
the CPU-side update/draw was ~8 ms but the main thread spent ~14-28 ms
waiting for the FPGA to finish the per-band submit/flush sequence.

Optimization: the HW backend now enables an HW-only submit
worker. `renderer_end_frame` keeps the same public order, but
`gpu_transport_submit_descriptors` hands the already-binned frame to the
worker and returns immediately. The following `gpu_transport_flip` call
is consumed as worker-owned; the worker performs the actual HW
submit+flip after draining the handed-off bins. The main thread can then
start CPU emit for frame N+1 while the FPGA drains frame N. Set
`VOXEL_PIPELINE_FRAMES=0` to return to synchronous submission.

Safety shape:

  * Enabled by default for `VOXEL_GPU_BACKEND=hw`; set
    `VOXEL_PIPELINE_FRAMES=0` to disable.
  * HW backend only (`VOXEL_GPU_BACKEND=hw`). Socket/tee stay synchronous
    because the socket backend consumes the contiguous `submit_buffer`,
    which the renderer reuses immediately on the next frame.
  * `gpu_transport_clear`, `gpu_transport_set_palette`, and
    `gpu_transport_set_fog` first wait for the prior worker job, so clear
    and GPU state writes never interleave with an in-flight band submit.
  * Worker errors are stored in the transport and surfaced on the next
    wait boundary (normally the next frame's `gpu_transport_clear`).

Mechanism:

  * `g_bins_pool[2][VOXEL_BAND_COUNT]` double-buffers the per-band
    descriptor bins. Main fills `g_main_bin_set`; a queued pipeline job
    records that set and flips `g_main_bin_set ^= 1` for the next frame.
  * `submit_hw_prebinned(...)` is the shared submit path for both sync and
    worker modes. It consumes an explicit bin-set pointer instead of
    reading a global, so the worker can submit frame N while main bins
    frame N+1.
  * The pipeline job snapshots staged diagnostics (`diag_cost`, bbox,
    frame index, quad count) before the main thread starts the next frame.
  * `gpu_transport_flip_hw_only` factors the HW flip path out of the
    public `gpu_transport_flip` so the worker can flip without touching
    socket state or recursively entering the public pipeline gate.

Files touched:

  * `sw/gpu_transport.c` -- pthread worker, double-buffered bin pool,
    explicit-bin submit helper, opt-in env handling, wait boundaries for
    clear/palette/fog, worker-owned flip path.

Expected impact: on frames where CPU emit/update is fully serialized
behind FPGA band submission, this can recover most of that CPU time
(roughly the ~8 ms/frame seen in recent logs). It cannot reduce the FPGA
work itself; it overlaps it.


May 8 2026: Tunable Frame Cap and Sky Palette Cadence
=====================================================

Context: Frame pipelining was stable on hardware. Logs showed cached/static
frames can drop to ~11-14 ms of CPU work, but the game loop still caps at
30 FPS. Logs also showed periodic `render_epoch`/`sky_epoch` bumps on
otherwise static views, which invalidates full-band reuse and forces a
full redraw for a sky/daylight palette tick.

Changes:

  * `VOXEL_TARGET_FPS` lets the main loop frame cap be raised without a
    rebuild. Default remains 30 FPS; accepted range is 15-120 FPS.
    Useful test command: `VOXEL_TARGET_FPS=60 VOXEL_PIPELINE_FRAMES=1`.
  * `DEFAULT_SKY_PALETTE_TIME_STEP_SECONDS` increased from 0.5 s to
    1.0 s, cutting periodic sky/daylight palette cache invalidations in
    half while still giving 180 sky states over the 180 s day cycle.
  * `VOXEL_SKY_PALETTE_STEP_SECONDS` (0.1-10.0) can tune that cadence.
    Larger values favor frame-cache reuse; smaller values favor smoother
    sky color motion.
  * Removed stale `sys/uio.h` / `IOV_MAX` leftovers from the rejected
    `writev` experiment.
  * Cleanup: ignored root `a.out` and one-off `test_band.*` Verilator
    scratch files; fixed `hw/Makefile`'s `tar` target by removing missing
    legacy `ip/intr_capturer`/`.srf` dependencies and correcting `.PHONY`.

Expected impact: this does not speed up active moving frames where the FPGA
must redraw the scene. It makes the new pipeline measurable above 30 FPS on
cache-hit/static frames and reduces periodic full-redraw spikes caused only
by sky palette motion.


May 8 2026: Lazy Color Clear via Z Sentinel
===========================================

Context: The older two-pixel `ST_CACHE_INIT` attempt failed Quartus fit by
43 LABs because it tried to clear color and Z through both banked cache write
lanes. Recent logs still show `init=6.14ms` on uncached full-frame redraws,
so the fixed init cost is worth attacking if we can avoid that area shape.

RTL change:

  * `ST_CACHE_INIT` now clears only the Z cache, two pixels/cycle, using the
    even/odd bank write ports directly.
  * `Z=0xFFFF` is reserved as `Z_CLEAR_SENTINEL`, meaning "no real pixel has
    written this cache location for this band." Real committed pixels write
    either their clamped depth or `0xFFFE` for non-Z-tested pixels so flush
    can distinguish valid color from untouched stale color.
  * The color cache is not physically initialized. When draw blending reads
    a destination with `Z_CLEAR_SENTINEL`, it substitutes the current sky or
    clear color. When background flush reads a cache pixel with
    `Z_CLEAR_SENTINEL`, it writes generated sky/clear color to SDRAM instead
    of stale cache color.
  * The old cache-init sky palette row counters and color write path were
    removed, so this should be much more fit-friendly than the previous
    two-pixel color+Z init attempt.

Low-risk instrumentation:

  * Added `VOXEL_IOC_GET_PERF2` while keeping the old `VOXEL_IOC_GET_PERF`
    ABI intact.
  * New RTL counters split background flush stalls into `load`, `fifo`,
    `data`, and `drain`; `gpu_transport.c` prints them as
    `renderer: hw_flush_wait ...` when the v2 ioctl is available.
  * The software bbox estimator now models cache init as two pixels/cycle.

Expected impact: uncached full-frame init should fall from ~6.14 ms to
~3.07 ms at 50 MHz. This does not reduce SDRAM flush bytes, but it should
help frames hovering just over the 16.80 ms one-vsync bucket.

Validation done locally:

  * `verilator --lint-only` passes with Intel megafunctions boxed/missing
    warnings suppressed.
  * `cc -Wall -Wextra -pthread -c sw/gpu_transport.c`, `sw/renderer.c`, and
    `sw/game.c` pass.
  * Renderer test binaries build; only the pre-existing `world.c` warnings
    remain.

Hardware validation still needed: rebuild the RBF in Quartus to confirm the
LAB fit, reload the kernel module so `VOXEL_IOC_GET_PERF2` is available, and
check board logs for `init~=3.07ms` on uncached redraws plus the new
`hw_flush_wait` breakdown.
