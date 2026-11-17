/*
 * Bare-metal entry for Redmi Note 8 Pro (begonia / MT6785).
 * Linked at 0x40080000 with an ARM64 Linux Image header so MediaTek LK jumps here
 * with MMU/caches off and x0 = live DTB physical address.
 */
.global _start
.global jump_to_kernel
.global chain_stub
.global chain_stub_end
.global psci_system_reset
.global psci_cpu_on

.section .text.boot, "ax"

_start:
    b       primary_entry           /* code0 */
    .word   0                       /* code1 */
    .quad   0x8000                  /* text_offset (BoardConfig.mk BOARD_KERNEL_OFFSET) */
    .quad   0x01000000              /* image_size (patched by pack_bootimg.py) */
    .quad   0x0a                    /* flags: LE, 4K pages, place anywhere */
    .quad   0                       /* res2: chained Android Image byte length (0 if standalone) */
    .quad   0                       /* res3: offset of appended SWEETXNU payload from _start */
    .quad   0                       /* res4 */
    .ascii  "ARM\x64"               /* magic */
    .word   0                       /* res5 */

primary_entry:
    msr     daifset, #0xf
    mov     x19, x0                 /* save live DTB PA from LK */

    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    adrp    x2, mtk_vectors
    add     x2, x2, :lo12:mtk_vectors
    cmp     x1, #2
    b.ne    1f
    msr     vbar_el2, x2
    b       2f
1:  msr     vbar_el1, x2
2:
    isb

    /* Clear .bss before setting up C stack inside BSS */
    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __bss_end
    add     x1, x1, :lo12:__bss_end
3:  cmp     x0, x1
    b.hs    4f
    str     xzr, [x0], #8
    b       3b
4:
    adrp    x0, _stack_top
    add     x0, x0, :lo12:_stack_top
    mov     sp, x0

    mov     x0, x19
    bl      mtkloader_main

5:  wfe
    b       5b

/*
 * jump_to_kernel(uint64_t entry_pa, uint64_t boot_args_pa)
 * Enter XNU at EL1h with MMU off, caches cleaned/invalidated, and x0 = boot_args_pa.
 */
jump_to_kernel:
    msr     daifset, #0xf
    mov     x19, x0
    mov     x20, x1

    bl      dcache_clean_all

    mrs     x2, CurrentEL
    lsr     x2, x2, #2
    cmp     x2, #2
    b.ne    1f

    /* Drop EL2 -> EL1h cleanly (AArch64 EL1, CNTVOFF_EL2 = 0) */
    msr     cntvoff_el2, xzr
    mov     x2, #(1 << 31)          /* HCR_EL2.RW */
    msr     hcr_el2, x2
    mov     x2, #0x3c5              /* EL1h, DAIF masked */
    msr     spsr_el2, x2
    msr     elr_el2, x19
    mov     x0, x20
    ic      iallu
    dsb     sy
    isb
    eret

1:  mrs     x2, sctlr_el1
    bic     x2, x2, #0x1            /* M: MMU off */
    bic     x2, x2, #0x4            /* C: D-cache off */
    bic     x2, x2, #0x1000         /* I: I-cache off */
    msr     sctlr_el1, x2
    isb
    ic      iallu
    dsb     sy
    isb
    mov     x0, x20
    br      x19

/*
 * chain_stub(uint64_t dtb, uint64_t dst, uint64_t src, uint64_t len, uint64_t entry)
 * Position-independent copier executed out of high DRAM to restore Android's original Image.
 */
chain_stub:
1:  ldr     x6, [x2], #8
    str     x6, [x1], #8
    subs    x3, x3, #8
    b.hi    1b
    dsb     sy
    ic      iallu
    dsb     sy
    isb
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    br      x4
chain_stub_end:

/* SMCCC PSCI helpers */
psci_system_reset:
    ldr     w0, =0x84000009
    smc     #0
1:  wfe
    b       1b

psci_cpu_on:
    ldr     x4, =0xc4000003
    mov     x3, x2
    mov     x2, x1
    mov     x1, x0
    mov     x0, x4
    smc     #0
    ret

/* Clean and invalidate every data/unified cache level to PoC by set/way. */
dcache_clean_all:
    mrs     x0, clidr_el1
    and     w3, w0, #0x07000000
    lsr     w3, w3, #23
    cbz     w3, 5f
    mov     w10, #0
1:  add     w2, w10, w10, lsr #1
    lsr     w1, w0, w2
    and     w1, w1, #7
    cmp     w1, #2
    b.lt    4f
    msr     csselr_el1, x10
    isb
    mrs     x1, ccsidr_el1
    and     w2, w1, #7
    add     w2, w2, #4
    ubfx    w4, w1, #3, #10
    clz     w5, w4
    ubfx    w7, w1, #13, #15
2:  mov     w9, w4
3:  lsl     w6, w9, w5
    orr     w11, w10, w6
    lsl     w6, w7, w2
    orr     w11, w11, w6
    dc      cisw, x11
    subs    w9, w9, #1
    b.ge    3b
    subs    w7, w7, #1
    b.ge    2b
4:  add     w10, w10, #2
    cmp     w3, w10
    b.gt    1b
5:  dsb     sy
    isb
    ret

.macro exc_slot id
    .balign 0x80
    mov     x0, #\id
    mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
    bl      loader_exception_handler
    b       .
.endm

    .balign 0x800
mtk_vectors:
    exc_slot 0
    exc_slot 1
    exc_slot 2
    exc_slot 3
    exc_slot 4
    exc_slot 5
    exc_slot 6
    exc_slot 7
    exc_slot 8
    exc_slot 9
    exc_slot 10
    exc_slot 11
    exc_slot 12
    exc_slot 13
    exc_slot 14
    exc_slot 15
