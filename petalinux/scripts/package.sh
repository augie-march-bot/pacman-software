#! /bin/bash

petalinux-package --boot --force --fpga --u-boot --fsbl ../../products/pacman_fsbl.elf

# TFTP boot script (ramdisk build only), from the deploy directory if petalinux did not copy it:
[ -f images/linux/boot.scr.uimg ] || cp -v build/tmp/deploy/images/*/boot.scr.uimg images/linux/ 2>/dev/null || true
