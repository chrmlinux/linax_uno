/* unox syscall gate: fixed at flash byte 0x2000 (word 0x1000).
 * 16 entries of JMP (4 bytes each). User programs call the entry
 * address as a function pointer; the JMP lands in the handler. */
#include <avr/io.h>

	.extern sys_exit
	.extern sys_read
	.extern sys_write
	.extern sys_yield
	.extern sys_getpid
	.extern sys_sleep
	.extern sys_exec
	.extern sys_wait
	.extern sys_open
	.extern sys_close
	.extern sys_readdir
	.extern sys_argv
	.extern sys_tasks
	.extern sys_openw
	.extern sys_gpio
	.extern sys_i2c
	.extern sys_unimpl

	.section .gate, "ax", @progbits
	.global unox_gate
unox_gate:
	jmp	sys_exit	/* 0 */
	jmp	sys_write	/* 1 */
	jmp	sys_read	/* 2 */
	jmp	sys_yield	/* 3 */
	jmp	sys_getpid	/* 4 */
	jmp	sys_sleep	/* 5 */
	jmp	sys_exec	/* 6 */
	jmp	sys_wait	/* 7 */
	jmp	sys_open	/* 8 */
	jmp	sys_close	/* 9 */
	jmp	sys_readdir	/* 10 */
	jmp	sys_argv	/* 11 */
	jmp	sys_tasks	/* 12 */
	jmp	sys_openw	/* 13 */
	jmp	sys_gpio	/* 14 */
	jmp	sys_i2c		/* 15 */
