#include "unox.h"

static int seed = 7;	/* .data: the kernel must copy this in */
static int accum;	/* .bss: the kernel must zero this */

int main(void)
{
	uputs("prog2 (pid ");
	uputd(sys_getpid());
	uputs(") seed ");
	uputd(seed);
	accum += seed;
	uputs(", accum ");
	uputd(accum);
	uputs("\n");
	seed++;
	sys_sleep(50);
	uputs("prog2 woke, seed now ");
	uputd(seed);
	uputs("\n");
	return accum - (seed - 1);
}
