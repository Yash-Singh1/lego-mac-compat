    .text
    .globl A, M, B, O, lp_a, lp_m, cs_a, cs_a_end, cs_m, cs_m_end, cs_r, cs_r_end, end
# A: catch(...) around M(); checks callee-saved registers survived unwinding.
A:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    subl $28, %esp
    movl $0x11111111, %ebx
    movl $0x22222222, %esi
    movl $0x33333333, %edi
cs_a:
    call M
cs_a_end:
    xorl %eax, %eax
    jmp 9f
lp_a:
    cmpl $0x11111111, %ebx
    jne 8f
    cmpl $0x22222222, %esi
    jne 8f
    cmpl $0x33333333, %edi
    jne 8f
    cmpl $1, %edx
    jne 8f
    movl %eax, (%esp)
    movl $0xc0de0001, %eax
    call *%eax
    movl (%eax), %eax
    movl %eax, 16(%esp)
    movl $0xc0de0002, %eax
    call *%eax
    movl 16(%esp), %eax
    jmp 9f
8:
    movl $-1, %eax
9:
    addl $28, %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
# M: a cleanup (destructor) around B(), then _Unwind_Resume.
M:
    pushl %ebp
    movl %esp, %ebp
    pushl %esi
    subl $20, %esp
    movl $0x66666666, %esi
cs_m:
    call B
cs_m_end:
    addl $20, %esp
    popl %esi
    popl %ebp
    ret
lp_m:
    movl %eax, (%esp)
    movl $0xc0de0007, %ecx
    incl (%ecx)
    movl $0xc0de0003, %ecx
cs_r:
    call *%ecx
cs_r_end:
    ud2
# B: throws an int-sized object holding 0x1234.
B:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %edi
    subl $16, %esp
    movl $0x44444444, %ebx
    movl $0x55555555, %edi
    movl $4, (%esp)
    movl $0xc0de0004, %eax
    call *%eax
    movl $0x1234, (%eax)
    movl %eax, (%esp)
    movl $0xc0de0008, 4(%esp)
    movl $0, 8(%esp)
    movl $0xc0de0005, %eax
    call *%eax
    ud2
# O: @try { @throw 0x777 } @catch -> returns the extracted exception.
O:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    subl $116, %esp
    leal 16(%esp), %ebx
    movl %ebx, (%esp)
    movl $0xc0de0009, %eax
    call *%eax
    movl %ebx, (%esp)
    movl $0xc0de000a, %eax
    call *%eax
    testl %eax, %eax
    jne 7f
    movl $0x777, (%esp)
    movl $0xc0de000b, %eax
    call *%eax
    ud2
7:
    movl %ebx, (%esp)
    movl $0xc0de000c, %eax
    call *%eax
    addl $116, %esp
    popl %ebx
    popl %ebp
    ret
end:
