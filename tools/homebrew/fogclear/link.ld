/* SPDX-License-Identifier: GPL-3.0-or-later */
ENTRY(_start9)
SECTIONS {
  . = 0x02000000;
  .text9 : { *(.start9) *(.text*) *(.rodata*) *(.data*) }
  .bss9 : { *(.bss*) *(COMMON) }
  . = 0x02380000;
  .text7 : { *(.start7) }
  /DISCARD/ : { *(.ARM.*) *(.comment) *(.note*) }
}
