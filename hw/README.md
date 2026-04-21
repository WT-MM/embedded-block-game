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
