/* unox context switch for ATmega328P.
 *
 * Frame layout on a switched-out stack, ascending from SP:
 *   r31, r30, ..., r2, r1, SREG, r0, PCH, PCL   (35 bytes)
 * The PC is pushed by the interrupt hardware (tick path) or by
 * the CALL instruction (yield path). Both paths share the save /
 * restore code and leave with RETI, which also re-enables
 * interrupts.
 */
#include <avr/io.h>

	.extern proc_sp		/* uint16_t proc_sp[NPROC] */
	.extern cur_pid		/* uint8_t cur_pid */
	.extern sched_tick	/* C: tick++, maybe pick next */
	.extern sched_switch	/* C: pick next + window swap */

	.text

	.macro SAVE_CTX
	push	r0
	in	r0, _SFR_IO_ADDR(SREG)
	push	r0
	push	r1
	eor	r1, r1
	push	r2
	push	r3
	push	r4
	push	r5
	push	r6
	push	r7
	push	r8
	push	r9
	push	r10
	push	r11
	push	r12
	push	r13
	push	r14
	push	r15
	push	r16
	push	r17
	push	r18
	push	r19
	push	r20
	push	r21
	push	r22
	push	r23
	push	r24
	push	r25
	push	r26
	push	r27
	push	r28
	push	r29
	push	r30
	push	r31
	.endm

	.macro RESTORE_CTX
	pop	r31
	pop	r30
	pop	r29
	pop	r28
	pop	r27
	pop	r26
	pop	r25
	pop	r24
	pop	r23
	pop	r22
	pop	r21
	pop	r20
	pop	r19
	pop	r18
	pop	r17
	pop	r16
	pop	r15
	pop	r14
	pop	r13
	pop	r12
	pop	r11
	pop	r10
	pop	r9
	pop	r8
	pop	r7
	pop	r6
	pop	r5
	pop	r4
	pop	r3
	pop	r2
	pop	r1
	pop	r0
	out	_SFR_IO_ADDR(SREG), r0
	pop	r0
	.endm

	/* Store SP into proc_sp[cur_pid]. Clobbers r18-r20, r26, r27
	 * (all already saved in the frame). r1 is zero here. */
	.macro STORE_SP
	in	r19, _SFR_IO_ADDR(SPL)
	in	r20, _SFR_IO_ADDR(SPH)
	lds	r18, cur_pid
	lsl	r18
	ldi	r26, lo8(proc_sp)
	ldi	r27, hi8(proc_sp)
	add	r26, r18
	adc	r27, r1
	st	X+, r19
	st	X, r20
	.endm

	.macro LOAD_SP
	lds	r18, cur_pid
	lsl	r18
	ldi	r26, lo8(proc_sp)
	ldi	r27, hi8(proc_sp)
	add	r26, r18
	adc	r27, r1
	ld	r19, X+
	ld	r20, X
	out	_SFR_IO_ADDR(SPH), r20
	out	_SFR_IO_ADDR(SPL), r19
	.endm

	/* Timer1 COMPA vector (vector 11 on the 328P) */
	.global __vector_11
__vector_11:
	SAVE_CTX
	STORE_SP
	call	sched_tick
	LOAD_SP
	RESTORE_CTX
	reti

	/* void ctx_switch(void): cooperative switch. The CALL here
	 * has already pushed the return PC, so the frame matches
	 * the interrupt path. yield is kept as an alias. */
	.global ctx_switch
	.global yield
ctx_switch:
yield:
	cli
	SAVE_CTX
	STORE_SP
	call	sched_switch
	LOAD_SP
	RESTORE_CTX
	reti
