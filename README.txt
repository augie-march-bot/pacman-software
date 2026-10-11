pacman-software
===============

Software supporting PACMAN card.

Hardware export file:
---------------------
The pacman-firmware repository produces the pacman.xsa file which must be placed (or symlinked) into:

pacman-software/hardware/pacman.xsa

Tools:
------

This repo is currently based on Vivado 2023.2.  The Vivado version is included in each major branch name.

Branches:
---------

zipline-2023.2:
   This is the pristine zipline software branch for Vivado 2023.2

working-on-3.<X>-zipline-2023.2:
   A branch of this form is an integration branch aimed at zipline
   version 3.<X>

Usage
-----

See docs/flow.txt for development workflows.

