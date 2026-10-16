#! /bin/bash

petalinux-build || { echo "ERROR: petalinux-build failed"; exit 1; }

echo "REMINDER: run package step next (images/linux/BOOT.BIN has not yet been updated...)"
