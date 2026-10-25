# Program entry: the kernel puts argc in a0 and argv in a1, with sp at the
# top of a fresh stack.
    .section .text.start, "ax"
    .globl _start
_start:
    mv      fp, zero
    call    main
    call    exit
1:  j       1b
