Hardware Notes
==============

Regenerating Qsys HDL on the Lab Ubuntu Machines
------------------------------------------------

Context
-------
The FPGA-side SDRAM controller IP had to be added from Quartus 19.1 because it
is not available in the newer Lite install we first opened. On the lab Ubuntu
machines, `qsys-generate` can fail while generating the SDRAM controller RTL
because Quartus's bundled Perl cannot find the standard `Getopt::Long` module.

The important symptom/cause split is:

  * `Failed to find module soc_system_fpga_sdram` is only a downstream symptom.
  * `Can't locate Getopt/Long.pm in @INC` is the real cause.

What is happening is:

  1. Quartus launches the SDRAM IP generator script.
  2. That script is supposed to emit a generated module named
     `soc_system_fpga_sdram`.
  3. The script crashes before generating RTL because Perl cannot load
     `Getopt::Long`.
  4. Quartus then reports that it cannot find `soc_system_fpga_sdram`.

So if you see the missing-module error together with the Perl error, do not
rename the IP or re-do the Qsys wiring. Fix the Perl environment and re-run HDL
generation.

One-Time Shell Setup for Quartus 19.1
-------------------------------------

Run these commands in the same shell you will use to launch Quartus:

```bash
unset PERL5LIB PERLLIB PERL_LOCAL_LIB_ROOT PERL_MB_OPT PERL_MM_OPT PERL5OPT

export PERL5LIB="$(
  /usr/bin/perl -e 'print join(":", grep { -d($_) && $_ !~ /x86_64-linux/ } @INC), "\n"'
)"

echo "$PERL5LIB"
```

Sanity Check
------------

Before opening Quartus, verify that Quartus's Perl can now see `Getopt::Long`:

```bash
/tools/intel/intelFPGA/19.1/quartus/linux64/perl/bin/perl -MGetopt::Long -e 'print "ok\n"'
```

If this prints:

```text
ok
```

then the Perl workaround is active for this shell.

GUI Flow
--------

Launch Quartus 19.1 from that same shell:

```bash
/tools/intel/intelFPGA/19.1/quartus/bin/quartus /homes/user/stud/fall25/wm2505/Github/embedded-block-game/hw/soc_system.qpf
```

Then in Quartus:

  1. Open `Tools -> Platform Designer`.
  2. Open `soc_system.qsys` if it does not open automatically.
  3. Make any needed Qsys edits.
  4. Click `Generate HDL...`.

The `hw/Makefile` also has helper targets that hardcode the lab's Quartus 19.1
tool paths:

  * `make qsys19`
  * `make project19`
  * `make quartus19`
  * `make rbf19`

CLI Alternative
---------------

To test HDL generation without reopening the GUI, run:

```bash
cd /homes/user/stud/fall25/wm2505/Github/embedded-block-game/hw
/tools/intel/intelFPGA/19.1/quartus/bin/qsys-generate soc_system.qsys \
  --synthesis=VERILOG \
  --output-directory=./soc_system/synthesis \
  --family="Cyclone V" \
  --part=5CSEMA5F31C6
```

If It Still Fails
-----------------

Re-run the sanity check:

```bash
/tools/intel/intelFPGA/19.1/quartus/linux64/perl/bin/perl -MGetopt::Long -e 'print "ok\n"'
```

If that does not print `ok`, the machine's Quartus/Perl environment is still
broken and the issue is not in the Qsys design itself.

End-to-End SDRAM Bring-Up on This Branch
----------------------------------------

What is already wired
---------------------
This branch already contains the first-stage FPGA SDRAM scaffolding:

  * `soc_system.qsys` instantiates `fpga_sdram` and connects
    `hps_0.h2f_axi_master -> fpga_sdram.s1`.
  * `soc_system_top.sv` exports the generated SDRAM pins onto the board
    `DRAM_*` signals and drives `DRAM_CLK`.
  * The software tree adds EXTMEM control/status ABI plus
    `sw/tests/fpga_sdram_test.c`, a raw `/dev/mem` smoke test for the HPS view
    of the SDRAM window.

What is not implemented yet
---------------------------
The GPU does not yet render into SDRAM. The current goal is only to prove that
the HPS can successfully access the FPGA-side SDRAM controller without hanging.

Recommended bring-up order
--------------------------

1. Regenerate Platform Designer output and handoff
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Run this from a shell that has both:

  * the Quartus 19.1 Perl workaround from above, and
  * the SoC embedded shell environment if you plan to build preloader/U-Boot.

Then regenerate Qsys output:

```bash
cd /homes/user/stud/fall25/wm2505/Github/embedded-block-game/hw
make qsys
```

This step matters for two reasons:

  * it regenerates the SDRAM controller HDL under `soc_system/`, and
  * it regenerates `hps_isw_handoff/soc_system_hps_0`, which is the handoff
    consumed by the SPL/preloader build.

If `make qsys` fails with `Failed to find module soc_system_fpga_sdram`, go back
to the Perl sanity check above. That missing-module error is not the root cause.

2. Rebuild the FPGA image
~~~~~~~~~~~~~~~~~~~~~~~~~
After a successful Qsys regeneration:

```bash
cd /homes/user/stud/fall25/wm2505/Github/embedded-block-game/hw
make quartus rbf
```

That produces an updated `soc_system.rbf`.

3. Follow the lab boot flow first: copy the `.rbf` onto the FAT boot partition
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
The Lab 3 handout that this project builds on uses the standard class flow:

  * build a new `.rbf`,
  * copy it onto the SD card's boot partition,
  * and reboot Linux from the existing card image.

The handout treats U-Boot as an optional debug shell that can manually load
`soc_system.rbf` and run `bridge_enable_handoff`; it does not require flashing a
new U-Boot image for normal FPGA-peripheral iterations.

So the first thing to try on this SDRAM branch is still:

```bash
cd /homes/user/stud/fall25/wm2505/Github/embedded-block-game/hw
make mount-boot
make install-built-rbf
```

If you also have a valid `soc_system.dtb`, copy that to the same boot partition.
For the raw `/dev/mem` SDRAM test, the `.dtb` is not strictly required.

4. Reboot Linux from the updated SD card image
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
The lab handout's normal path is to boot Linux with the updated `.rbf`
(and usually the updated `.dtb`) already present on the boot partition.

If the board is already running Linux, prefer a clean `reboot` first. The handout
specifically notes that this is the normal iteration path and treats a manual
U-Boot `.rbf` load as an optional debugging step.

5. Run the smallest possible SDRAM smoke test first
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Rebuild the software tests if needed:

```bash
cd /Users/wesleymaa/Columbia/embedded_csee4840/embedded-block-game/sw
make -B tests/fpga_sdram_test tests/voxel_test
```

Then start with a tiny access window instead of the default 64 MiB sweep:

```bash
sudo ./tests/fpga_sdram_test 0xC0000000 0x1000
```

On this project, `0xC0000000` is the expected full HPS-to-FPGA bridge base.
The SDRAM slave is currently at offset `0x0000` inside that bridge window, so
the first test should touch that base directly.

If this 4 KiB test passes, only then scale the span upward.

6. Use U-Boot only as an isolation/debug path
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
If Linux-side testing still hangs, drop into U-Boot and follow the lab handout's
style of manual testing:

  * `fatload mmc 0:1 $fpgadata soc_system.rbf`
  * `fpga load 0 $fpgadata $filesize`
  * `run bridge_enable_handoff`

That is useful to separate "Linux/device-tree problem" from "bridge or hardware
problem." For this SDRAM branch, the next experiment after enabling handoff is to
probe the bridge cautiously, starting with the known-working lightweight path
before attempting the full HPS-to-FPGA SDRAM window.

7. Treat DTB regeneration as secondary during first bring-up
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
The current raw `/dev/mem` test does not depend on the device tree. It is okay
to defer DT cleanup if `sopc2dts` still trips over `altera_up_avalon_sys_sdram_pll`
or emits bad `sdram_clocks` phandles.

8. Escalate to rebuilding preloader/U-Boot only if the lab flow still fails
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Rebuilding the SPL/preloader and U-Boot from regenerated handoff remains a valid
escalation path:

```bash
cd /homes/user/stud/fall25/wm2505/Github/embedded-block-game/hw
make preloader uboot
```

However, the Lab 3 flow indicates this should not be the first assumption. Use
it when you have evidence that the existing boot chain is not enabling or handing
off the bridges the way the current `soc_system.qsys` expects.

Why this ordering matters
-------------------------
If the first HPS access to `0xC0000000` still hard-hangs the SoC after following
the standard lab boot flow, focus next on bridge/runtime configuration and reset
topology, not on larger software changes.

The most likely places to inspect are:

  * whether the board really booted the updated `soc_system.rbf`,
  * whether `bridge_enable_handoff` is enabling the full bridge path you need,
  * whether the full HPS-to-FPGA bridge is enabled at runtime,
  * whether the generated handoff/base addresses match the image you loaded,
  * and whether the dual reset fan-in on `fpga_sdram.reset` /
    `voxel_gpu_0.reset` is behaving as intended.

FPGA-Local SDRAM Self-Test Mode
-------------------------------

For isolating the board SDRAM itself from the HPS bridge and boot-chain state,
this branch also includes a pure FPGA-side test path:

  * `soc_system_top.sv` now lets a local `sdram_selftest_vga` block own the
    board `DRAM_*` and `VGA_*` pins.
  * `sdram_selftest_vga.sv` wraps the Terasic SDRAM controller plus a reduced
    write/readback test over the first 1024 words.
  * The test auto-starts after reset and draws `SDRAM TEST` plus one of
    `RESET`, `WRITE`, `READ`, `PASS`, or `FAIL` on the VGA output.

What to expect on hardware
--------------------------

  * `KEY[0]` acts as a manual reset/restart for the local test.
  * `LEDR[0]` lights while the test is running.
  * `LEDR[1]` lights on pass.
  * `LEDR[2]` lights on fail.
  * `LEDR[6:3]` show the raw `RW_Test` FSM state.

How to run this mode
--------------------

Build and install the bitstream exactly like the normal lab flow:

```bash
cd /homes/user/stud/fall25/wm2505/Github/embedded-block-game/hw
make quartus19 rbf19
make mount-boot
make install-built-rbf
```

Then reboot the board. For this local self-test path, you do not need
`bridge_enable_handoff`, `/dev/mem`, or the Linux smoke test at all. If the
VGA screen reaches `PASS`, the SDRAM pins, clocking, controller timing, and
basic read/write path are all working independently of the HPS bridge.

One subtlety
------------

`soc_system.qsys` still contains the experimental HPS-connected SDRAM block, but
the top-level board SDRAM pins are currently driven by the local test wrapper
instead. That keeps the HPS experiment in-tree while letting bring-up proceed on
the simpler FPGA-only path first.
