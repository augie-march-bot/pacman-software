pacman_fwcheck:  the firmware requirements, checked in the installed firmware
=============================================================================

Written by augie (AI agent).

The firmware requirements live in pacman-firmware ghdl/agents/requirements.txt:  what each unit promises (level
1), testable statements (level 2), and the checks behind them (level 3:  the GHDL checks, "make check").  The
level-2 requirements marked "sim+hw" (30 of 53) can also be checked on the board, with no tiles and no external
signals:  pacman_fwcheck does that, on Linux or bare metal, from one source (fwcheck.c).

What it does
  read-only (always):  the version registers, STATUS, every unit answering, undefined addresses timing out with
                       0xEEEEEEEE, the timestamp rate
  the rest:            register read-back and crosstalk, broadcast writes;  DMA packets out of TX, through the
                       loopback inside the firmware (RX INPUT 2), back through RX and DMA:  masks, all channels,
                       every bit, 1000 random packets, frame length, headers, packet size and timeout;  the test
                       pattern;  heartbeat and marker words;  SYNC;  the timing unit's counters
Output:  failed checks as they happen (all checks with -v), then one line per requirement and a VERDICT line
(PASS, FAIL, INCOMPLETE when some requirement could not be checked).  Linux exit code:  0 pass, 1 fail, 2 incomplete (e.g. read-only), 3 usage.

Safety
  - Only the read-only checks run when J5 is installed, when tile power or any tile is enabled, with --read_only,
    or (Linux) while a pacman server runs (it owns DMA and the RX/TX configuration):
        /etc/init.d/pacman_server stop;  ./pacman_fwcheck;  /etc/init.d/pacman_server start
  - Every register written is saved first and restored at the end (also on Ctrl-C or kill, Linux).  Not restored:
    the RX/TX and timing-unit counters, the timestamp (SYNC restarts it), and the DMA rings (reset;  the servers
    set them up again when started).
  - Nothing leaves the PL:  tiles stay off, and the timing-unit checks route pokes to markers only, not to SYNC or
    TRIG.

Build and run
  Linux:       make            build/pacman_fwcheck, static (Vitis arm-linux-gnueabihf-gcc)
               make stage      to /srv/tftp/trenz/dev;  on the board:  tftp -g -r dev/pacman_fwcheck 10.42.0.1
  bare metal:  xsct agents/fwcheck/baremetal/install.tcl        (from the top of pacman-software;  the Vitis
               workspace is agents/fwcheck/build/bm, against hardware/pacman.xsa)
               cd agents/fwcheck/build/bm && xsct run.tcl     (JTAG:  resets the board;  output on the console)
  make reqcheck   the requirement list in fwcheck.c matches the sim+hw requirements of requirements.txt

First results (2026-10-17, bench Trenz, firmware 3.6.0 candidate 13dc480):  all 30 requirements pass on Linux
(twice, registers restored) and bare metal (twice);  a run takes 0.2 s on Linux.
