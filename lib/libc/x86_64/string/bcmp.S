/*
 * $NetBSD: bcmp.S,v 1.1 2001/06/19 00:25:04 fvdl Exp $
 * $FreeBSD: src/lib/libc/amd64/string/bcmp.S,v 1.3 2008/11/02 01:10:54 peter Exp $
 */

#include <machine/asm.h>

ENTRY(bcmp)
	cld				/* set compare direction forward */

	movq	%rdx,%rcx		/* compare by words */
	shrq	$3,%rcx
	repe
	cmpsq
	jne	L1

	movq	%rdx,%rcx		/* compare remainder by bytes */
	andq	$7,%rcx
	repe
	cmpsb
L1:
	setne	%al
	movsbl	%al,%eax
	ret
END(bcmp)

	.section .note.GNU-stack,"",%progbits
