#
# xsct apps/jtag_boot/install.tcl
#

if {[file exists "jtag_boot"] == 1} {
    if {[file exists "jtag_boot.old"] == 1} {
	file delete -force jtag_boot.old
    }
    file rename jtag_boot/ jtag_boot.old/
}
exec mkdir jtag_boot

# Copy project TCL files:
file copy apps/jtag_boot/run.tcl jtag_boot/
file copy apps/jtag_boot/boot.tcl jtag_boot/
file copy apps/jtag_boot/boot_tftp.tcl jtag_boot/

# Set SDK workspace
cd jtag_boot
setws .


# Create hello world and fsbl apps:

app create -name fsbl_jtag -hw ../hardware/pacman.xsa -os standalone -proc ps7_cortexa9_0 -lang C -template {Zynq FSBL}
# the production FSBL sources (apps/pacman_fsbl/src), built as the JTAG FSBL:  PACMAN_JTAG_FSBL forces JTAG mode
# and calls the Trenz hook (MAC, Ethernet PHY, CPLD LEDs) before the JTAG exit.
importsources -name fsbl_jtag -path ../apps/pacman_fsbl/src/
app config -name fsbl_jtag define-compiler-symbols PACMAN_JTAG_FSBL

#build:
app build fsbl_jtag


