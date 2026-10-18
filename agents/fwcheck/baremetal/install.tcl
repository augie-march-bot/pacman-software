# pacman_fwcheck, bare metal:  a standalone Vitis app "fwcheck" in workspace agents/fwcheck/build/bm (run from the
# top of pacman-software):  the pacman-apps drivers as the bare-metal menu uses them (apps/menu/src), and fwcheck.c.
#   xsct agents/fwcheck/baremetal/install.tcl
#   cd agents/fwcheck/build/bm && xsct run.tcl        (output on the serial console;  this resets the board)
set ws agents/fwcheck/build/bm
if {[file exists $ws] == 1} {
    file delete -force $ws
}
file mkdir agents/fwcheck/build
setws $ws
app create -name fwcheck -hw hardware/pacman.xsa -os standalone -proc ps7_cortexa9_0 -lang C -template {Empty Application(C)}
foreach f {hw_access.c hw_access.h global.c global.h rxtx.c rxtx.h rxtx_menu.c rxtx_menu.h dma.c dma.h jumper.c jumper.h
           asic.c asic.h atc.h iic.h} {
    file copy -force apps/menu/src/$f $ws/fwcheck/src/
}
file copy -force agents/fwcheck/fwcheck.c $ws/fwcheck/src/
file copy -force agents/fwcheck/baremetal/run.tcl $ws/
app build fwcheck
