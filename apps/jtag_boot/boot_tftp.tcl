# boot_tftp.tcl:  as boot.tcl, but Linux and firmware come from a TFTP server:  loads the device tree, U-Boot,
# and the TFTP boot script boot.scr.uimg at scriptaddr (0x3000000).  U-Boot's autoboot (boot target jtag) runs the
# script:  dhcp, system.bit -> fpga loadb, image.ub -> bootm.  Nothing to type.  Run after run.tcl, like boot.tcl.
# Needs on the TFTP server:  system.bit, image.ub (petalinux/ramdisk/images/linux).
setws .
connect
targets -set -nocase -filter {name =~ "ARM*#0"}

# From petatlinux config file:
# CONFIG_SUBSYSTEM_UBOOT_DEVICETREE_OFFSET="0x100000"
set DEVICETREE_OFFSET 0x100000
dow -data "../petalinux/ramdisk/images/linux/system.dtb" ${DEVICETREE_OFFSET}

dow "../petalinux/ramdisk/images/linux/u-boot.elf"

# U-Boot scriptaddr (boot target jtag sources the script found there):
dow -data "../petalinux/ramdisk/images/linux/boot.scr.uimg" 0x3000000

con
