/* Program images, placed at fixed flash slots by the linker
 * (--section-start). Each image is a 16-byte header + the
 * program's own binary. Symbols are flash byte addresses. */
	.section .prog1, "a", @progbits
	.global prog1_img
prog1_img:
	.incbin "prog1.img"
	.section .prog2, "a", @progbits
	.global prog2_img
prog2_img:
	.incbin "prog2.img"
