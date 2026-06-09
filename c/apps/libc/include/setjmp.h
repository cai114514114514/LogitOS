#ifndef _SETJMP_H
#define _SETJMP_H

/* jmp_buf holds the callee-saved regs + rsp + return address (see setjmp.asm):
 * [0]=rbx [1]=rbp [2]=r12 [3]=r13 [4]=r14 [5]=r15 [6]=rsp [7]=rip */
typedef long jmp_buf[8];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif /* _SETJMP_H */
