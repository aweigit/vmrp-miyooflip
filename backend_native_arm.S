    .syntax unified
    .text
    .align 2
    .global vmrp_native_arm_call
    .type vmrp_native_arm_call, %function
vmrp_native_arm_call:
    push    {r4-r11, lr}
    str     sp, [r0, #68]
    str     r1, [r0, #72]
    ldr     sp, [r0, #52]
    ldr     lr, =1f
    ldr     r2, [r0, #64]
    msr     cpsr_f, r2
    sub     sp, sp, #4
    ldr     r2, [r0, #72]
    str     r2, [sp]
    ldmia   r0, {r0-r12}
    pop     {pc}
1:
    ldr     lr, =active_backend
    ldr     lr, [lr]
    stmia   lr, {r0-r12}
    mrs     r0, cpsr
    str     r0, [lr, #64]
    str     sp, [lr, #52]
    ldr     sp, [lr, #68]
    pop     {r4-r11, pc}
    .size vmrp_native_arm_call, .-vmrp_native_arm_call
