/* The romfs image, placed at a fixed flash address by the
 * linker (--section-start=.romfs). Built by build_m3.py. */
	.section .romfs, "a", @progbits
	.global romfs_img
romfs_img:
	.incbin "romfs.img"
