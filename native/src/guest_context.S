/* Copied into the i386 bridge page. All internal branches are relative.
   jmp_buf is opaque: our 40-byte state fits Darwin i386's 72-byte buffer.
   Native frames cannot be unwound by a jump across a host-to-guest callback. */
.text
.code32
.globl _lp32_context_start, _lp32_context_end
.globl _lp32_context_setjmp, _lp32_context_fast_setjmp, _lp32_context_sigsetjmp
.globl _lp32_context_longjmp, _lp32_context_fast_longjmp
.globl _lp32_context_save_helper, _lp32_context_restore_helper
_lp32_context_start:
_lp32_context_setjmp:
    movl $1, %edx
    jmp 1f
_lp32_context_fast_setjmp:
    xorl %edx, %edx
    jmp 1f
_lp32_context_sigsetjmp:
    movl 8(%esp), %edx
1:
    movl 4(%esp), %ecx
    movl %ebx, 0(%ecx)
    movl %ebp, 4(%ecx)
    movl %esi, 8(%ecx)
    movl %edi, 12(%ecx)
    leal 4(%esp), %eax
    movl %eax, 16(%ecx)
    movl (%esp), %eax
    movl %eax, 20(%ecx)
    fnstcw 24(%ecx)
    stmxcsr 28(%ecx)
    movl %edx, 36(%ecx)
    testl %edx, %edx
    jz 2f
    pushl %edx
    pushl $0
    pushl %ecx
    .byte 0xb8 /* mov immediate helper address, patched when installed */
_lp32_context_save_helper:
    .long 0
    call *%eax
    addl $12, %esp
2:
    xorl %eax, %eax
    ret
_lp32_context_longjmp:
    movl 4(%esp), %ecx
    cmpl $0, 36(%ecx)
    je _lp32_context_fast_longjmp
    pushl $0
    pushl $1
    pushl %ecx
    .byte 0xb8
_lp32_context_restore_helper:
    .long 0
    call *%eax
    addl $12, %esp
_lp32_context_fast_longjmp:
    movl 4(%esp), %ecx
    movl 8(%esp), %eax
    testl %eax, %eax
    jne 3f
    incl %eax
3:
    movl 0(%ecx), %ebx
    movl 4(%ecx), %ebp
    movl 8(%ecx), %esi
    movl 12(%ecx), %edi
    fldcw 24(%ecx)
    ldmxcsr 28(%ecx)
    movl 20(%ecx), %edx
    movl 16(%ecx), %esp
    jmp *%edx
_lp32_context_end:
.code64
