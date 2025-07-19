	.file	"oCallDraw.c"
	.text
	.globl	_drawtext
	.data
	.align 4
_drawtext:
	.long	4313920
	.globl	_drawsprite
	.align 4
_drawsprite:
	.long	4281728
	.globl	_drawrect
	.align 4
_drawrect:
	.long	4281424
  .globl	_createTexFromFileInMemory
	.align 4
_createTexFromFileInMemory:
	.long	4969168
	.text
	.globl	_CallDrawText
	.def	_CallDrawText;	.scl	2;	.type	32;	.endef
_CallDrawText:
LFB15:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$56, %esp
	movl	_drawtext, %eax
	movl	48(%ebp), %edx
	movl	%edx, 32(%esp)
	movl	44(%ebp), %edx
	movl	%edx, 28(%esp)
	movl	12(%ebp), %edx
	movl	%edx, 24(%esp)
	movl	40(%ebp), %edx
	movl	%edx, 20(%esp)
	movl	36(%ebp), %edx
	movl	%edx, 16(%esp)
	movl	28(%ebp), %edx
	movl	%edx, 12(%esp)
	movl	32(%ebp), %edx
	movl	%edx, 8(%esp)
	movl	24(%ebp), %edx
	movl	%edx, 4(%esp)
	movl	20(%ebp), %edx
	movl	%edx, (%esp)
  movl  %eax, %edx
	movl	8(%ebp), %eax
	movl	16(%ebp), %ecx
	call	*%edx
  addl  $48, %esp
	movl	$1, %eax
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
LFE15:
	.globl	_CallDrawSprite
	.def	_CallDrawSprite;	.scl	2;	.type	32;	.endef
_CallDrawSprite:
LFB16:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$56, %esp
	movl	_drawsprite, %eax
	movl	56(%ebp), %edx
	movl	%edx, 44(%esp)
	movl	52(%ebp), %edx
	movl	%edx, 40(%esp)
	movl	48(%ebp), %edx
	movl	%edx, 36(%esp)
	movl	44(%ebp), %edx
	movl	%edx, 32(%esp)
	movl	40(%ebp), %edx
	movl	%edx, 28(%esp)
	movl	36(%ebp), %edx
	movl	%edx, 24(%esp)
	movl	32(%ebp), %edx
	movl	%edx, 20(%esp)
	movl	28(%ebp), %edx
	movl	%edx, 16(%esp)
	movl	24(%ebp), %edx
	movl	%edx, 12(%esp)
	movl	20(%ebp), %edx
	movl	%edx, 8(%esp)
	movl	16(%ebp), %edx
	movl	%edx, 4(%esp)
	movl	12(%ebp), %edx
	movl	%edx, (%esp)
	movl	8(%ebp), %edx
	call	*%eax
  addl  $48, %esp
	movl	$1, %eax
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
LFE16:
	.globl	_CallDrawRect
	.def	_CallDrawRect;	.scl	2;	.type	32;	.endef
_CallDrawRect:
LFB17:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$56, %esp
	movl	_drawrect, %eax
	movl	40(%ebp), %edx
	movl	%edx, 32(%esp)
	movl	36(%ebp), %edx
	movl	%edx, 28(%esp)
	movl	32(%ebp), %edx
	movl	%edx, 24(%esp)
	movl	28(%ebp), %edx
	movl	%edx, 20(%esp)
	movl	24(%ebp), %edx
	movl	%edx, 16(%esp)
	movl	20(%ebp), %edx
	movl	%edx, 12(%esp)
	movl	16(%ebp), %edx
	movl	%edx, 8(%esp)
	movl	12(%ebp), %edx
	movl	%edx, 4(%esp)
	movl	8(%ebp), %edx
	movl	%edx, (%esp)
  movl  %eax, %edx
  xor   %eax, %eax
	call	*%edx
  addl  $36, %esp
	movl	$1, %eax
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
LFE17:
	.globl	_loadTextureFromMemory
	.def	_loadTextureFromMemory;	.scl	2;	.type	32;	.endef
_loadTextureFromMemory:
LFB18:
	.cfi_startproc
	pushl	%ebp
	.cfi_def_cfa_offset 8
	.cfi_offset 5, -8
	movl	%esp, %ebp
	.cfi_def_cfa_register 5
	subl	$24, %esp
	movl	_createTexFromFileInMemory, %eax
	movl	24(%ebp), %edx
	movl	%edx, 12(%esp)
	movl	20(%ebp), %edx
	movl	%edx, 8(%esp)
	movl	16(%ebp), %edx
	movl	%edx, 4(%esp)
	movl	12(%ebp), %edx
	movl	%edx, (%esp)
	movl  %eax, %edx
	movl  8(%ebp), %eax
	call	*%edx
	leave
	.cfi_restore 5
	.cfi_def_cfa 4, 4
	ret
	.cfi_endproc
LFE18:
	.ident	"GCC: (i686-win32-dwarf-rev0, Built by MinGW-W64 project) 8.1.0"
