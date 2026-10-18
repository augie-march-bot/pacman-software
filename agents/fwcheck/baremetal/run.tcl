# pacman_fwcheck, bare metal:  load the bitstream and run fwcheck over JTAG (from the fwcheck workspace).
setws .
connect
targets -set -nocase -filter {name =~ "ARM*#0"}
rst -system
fpga pacman/hw/pacman.bit
source pacman/hw/ps7_init.tcl
ps7_init
ps7_post_config
dow fwcheck/Debug/fwcheck.elf
con
