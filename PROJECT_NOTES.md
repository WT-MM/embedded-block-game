Project Notes
=============

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
  * `cache_band_index`: resident vertical band, `pixel_y[8:6]`.

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

The last band is partial: band 7 covers rows 448..479. Flush/load logic must
transfer only 32 rows for that band, not the full 64 rows, otherwise it will
write past the color frame into the next SDRAM region.

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
        write() descriptors → kernel pushes into 2048-word FIFO, blocks when full
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
