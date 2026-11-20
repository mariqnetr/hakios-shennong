_Head:
  adr   x1, _Payload    /* Set _Entry address */
  b     _Start          /* Jump to the real code */

#if REQUIRES_KERNEL_HEADER == 1
_StackBase:
  .quad FD_BASE         /* Text Offset */

_StackSize:
  .quad FD_SIZE         /* Image Size */
  .quad 0               /* Flags */
  .quad 0               /* Reserved */
  .quad 0               /* Reserved */
  .quad 0               /* Reserved */
  .ascii "ARM\x64"      /* ARM64 Magic */
  .long 0               /* Reserved */
#endif

_Start:
  mov   x4, x1
  ldr   x5, =FD_BASE
  cmp   x4, x5
  beq   _Entry
  ldr   x6,  =FD_SIZE

  /*
   * Whatever the bootloader left dirty in the caches over where we copy to must go first: we copy with the
   * caches off, and such a line, written back later, lands over what we wrote. ABL loading a boot partition from
   * the flash (not `fastboot boot`) leaves such lines: seen as scattered changes in the first 8 MB of the payload.
   */
  mrs   x7, ctr_el0
  ubfx  x7, x7, #16, #4
  mov   x8, #4
  lsl   x7, x8, x7      /* the smallest data cache line */
  mov   x8, x5
  add   x9, x5, x6
_FlushFd:
  dc    civac, x8
  add   x8, x8, x7
  cmp   x8, x9
  b.lo  _FlushFd
#ifdef PAYLOAD_BASE
  movz  x8, #(PAYLOAD_BASE >> 16), lsl #16
  movz  x9, #0x400, lsl #16     /* the whole payload window, 64 MiB */
  add   x9, x8, x9
_FlushPayload:
  dc    civac, x8
  add   x8, x8, x7
  cmp   x8, x9
  b.lo  _FlushPayload
#endif
  dsb   sy
#ifdef PAYLOAD_BASE
  /* when the bootloader handed over: the system counter, for the loader's "time since power-on" (x2, x3 are free here) */
  movz  x8, #(PAYLOAD_BASE >> 16), lsl #16
  movz  x9, #0x3ff, lsl #16
  movk  x9, #0xe000
  add   x8, x8, x9              /* PAYLOAD_BASE + 0x3ffe000: past the panic log, before the last page */
  movz  x2, #0x4b48
  movk  x2, #0x4853, lsl #16
  movk  x2, #0x4d49, lsl #32
  movk  x2, #0x3130, lsl #48    /* "HKSHIM01" */
  mrs   x3, cntpct_el0
  stp   x2, x3, [x8]
  dc    civac, x8
  dsb   sy
#endif

_CopyLoop:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _CopyLoop

#ifdef PAYLOAD_BASE
  /* x4 now points just past the FD: an optional "XNUP" payload may follow */
  ldr   w2, [x4]
  movz  w3, #0x4E58
  movk  w3, #0x5055, lsl #16
  cmp   w2, w3
  b.ne  _NoPayload
  ldr   w6, [x4, #4]    /* Payload Size */
  add   x6, x6, #23     /* Header + Round Up to 16 */
  and   x6, x6, #~15
  movz  x5, #(PAYLOAD_BASE >> 16), lsl #16

_PayloadLoop:
  ldp   x2, x3, [x4], #16
  stp   x2, x3, [x5], #16
  subs  x6, x6, #16
  b.ne  _PayloadLoop

_NoPayload:
#endif
  ldr   x5, =FD_BASE

_Entry:
  br    x5

_Dead:
  b     _Dead

.text
.align 4

_Payload:
