.text
.org 0x6950
f6950: nop
.org 0x7c80
f7c80: nop
.org 0x7fe0
  stp x29, x30, [sp, #-0x20]!
  str x19, [sp, #0x10]
  mov x19, x0
  ldrb w8, [x0, #0x621]
  cbnz w8, L
  bl f7c80
L:
  add x0, x19, #0x200
  bl f6950
  ldr x19, [sp, #0x10]
  eor w0, w0, #1
  ldp x29, x30, [sp], #0x20
  ret
