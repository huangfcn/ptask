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
  mov 0x00(%rsp), %rdx
  lea 0x08(%rsp), %rcx         
  mov %r12,       0x00(%rdi)
  mov %r13,       0x08(%rdi)
  mov %r14,       0x10(%rdi)
  mov %r15,       0x18(%rdi)
  mov %rdx,       0x20(%rdi)  
  mov %rcx,       0x28(%rdi)
  mov %rbx,       0x30(%rdi)
  mov %rbp,       0x38(%rdi)

  mov 0x00(%rsi), %r12
  mov 0x08(%rsi), %r13
  mov 0x10(%rsi), %r14
  mov 0x18(%rsi), %r15
  mov 0x20(%rsi), %rax
  mov 0x28(%rsi), %rcx
  mov 0x30(%rsi), %rbx
  mov 0x38(%rsi), %rbp
  mov %rcx,       %rsp
  jmpq *%rax


  .align 32
  .global save_context
save_context:
  mov 0x00(%rsp), %rdx
  lea 0x08(%rsp), %rcx         
  mov %r12,       0x00(%rdi)
  mov %r13,       0x08(%rdi)
  mov %r14,       0x10(%rdi)
  mov %r15,       0x18(%rdi)
  mov %rdx,       0x20(%rdi)  
  mov %rcx,       0x28(%rdi)
  mov %rbx,       0x30(%rdi)
  mov %rbp,       0x38(%rdi)
  ret

  .align 32
  .global goto_context
goto_context:
  mov 0x00(%rdi), %r12
  mov 0x08(%rdi), %r13
  mov 0x10(%rdi), %r14
  mov 0x18(%rdi), %r15
  mov 0x20(%rdi), %rax
  mov 0x28(%rdi), %rcx
  mov 0x30(%rdi), %rbx
  mov 0x38(%rdi), %rbp
  mov %rcx,       %rsp
  jmpq *%rax
