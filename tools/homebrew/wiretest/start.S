@ SPDX-License-Identifier: GPL-3.0-or-later
@ Entry points of the wireframe test: the ARM9 sets a stack and runs the C
@ program; the ARM7 has nothing to do and spins.
    .arm
    .section .start9, "ax"
    .global _start9
_start9:
    ldr sp, =0x023F0000
    bl wire_main
1:  b 1b
    .ltorg
    .section .start7, "ax"
    .global _start7
_start7:
1:  b 1b
