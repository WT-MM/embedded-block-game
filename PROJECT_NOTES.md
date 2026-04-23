Project Notes
=============

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
The bug was in `hw/voxel_gpu.sv` inside the edge-function evaluation logic.
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
signed 64-bit arithmetic. The current implementation in `hw/voxel_gpu.sv`
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
In `hw/voxel_gpu.sv`, the palette storage was declared with

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
at `hw/voxel_gpu.sv`:

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
