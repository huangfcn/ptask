  .text

  .align 32
  .global asm_taskmain
asm_taskmain:  
  # setup one parameter
  movq %r12, %rdi
  jmpq *%r15

  .align 32
  .global swap_context
swap_context:
  # switch RSP
  mov %rsp, 0x28(%rdi)
  mov 0x28(%rsi), %rsp

  # save ip
  popq %rax

  mov %r12,       0x00(%rdi)
  mov %r13,       0x08(%rdi)
  mov %r14,       0x10(%rdi)
  mov %r15,       0x18(%rdi)

  mov %rbx,       0x30(%rdi)
  mov %rbp,       0x38(%rdi)

  mov 0x00(%rsi), %r12
  mov 0x08(%rsi), %r13
  mov 0x10(%rsi), %r14
  mov 0x18(%rsi), %r15

  mov 0x30(%rsi), %rbx
  mov 0x38(%rsi), %rbp

  jmpq *%rax

  # goto for thread maintask 
  # setup stack & save registers, 
  # so we will be able to return thread safely
  .align 32
  .global goto_contxt2
goto_contxt2:
  # save r12 and r15 into stack
  pushq %r12
  pushq %r15
  
  # make a return addres
  lea ret_here(%rip), %rax
  pushq %rax

  # update sp
  lea 0x00(%rsp), %rax
  mov %rax, 0x28(%rdi)

  # get cpp_main & args
  mov 0x18(%rdi), %rax
  mov 0x00(%rdi), %rdi
  jmp *%rax

ret_here:
  popq %r15
  popq %r12
  ret