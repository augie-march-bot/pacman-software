# PACMAN (ramdisk build):  also build the TFTP boot script boot.scr.uimg (see boot.cmd.tftp), next to boot.scr.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://boot.cmd.tftp"

do_compile:append() {
    mkimage -A arm -T script -C none -n "PACMAN TFTP boot" -d "${WORKDIR}/boot.cmd.tftp" boot.scr.uimg
}

do_deploy:append() {
    install -m 0644 boot.scr.uimg ${DEPLOYDIR}/boot.scr.uimg
}
