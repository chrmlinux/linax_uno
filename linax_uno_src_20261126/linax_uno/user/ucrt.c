/* unox userland runtime: entry trampoline + print helpers.
 * The kernel prepares .data/.bss and the stack, then jumps to
 * _start. Strings live in flash; uputs copies them through a
 * small RAM buffer because sys_write takes data-space buffers. */
#include <avr/pgmspace.h>
#include <string.h>
#include "unox.h"

extern int main(void);

void _start(void)
{
	sys_exit(main());
}

void uputs(const char *flash_str)
{
	char buf[16];
	uint8_t n = 0;
	char c;

	while ((c = (char)pgm_read_byte(flash_str++)) != 0) {
		buf[n++] = c;
		if (n == sizeof(buf)) {
			sys_write(1, buf, n);
			n = 0;
		}
	}
	if (n)
		sys_write(1, buf, n);
}

void uputd(int v)
{
	char buf[8];
	int i = 0;
	unsigned int u;

	if (v == 0) {
		char z = '0';

		sys_write(1, &z, 1);
		return;
	}
	if (v < 0) {
		char m = '-';

		sys_write(1, &m, 1);
		u = (unsigned int)(-v);
	} else {
		u = (unsigned int)v;
	}
	while (u) {
		buf[i++] = (char)('0' + u % 10);
		u /= 10;
	}
	while (i) {
		char c = buf[--i];

		sys_write(1, &c, 1);
	}
}

/* Compare a RAM string with a flash-resident literal. */
int ucmp(const char *ram, const char *flash_str)
{
	for (;;) {
		char a = *ram++;
		char b = (char)pgm_read_byte(flash_str++);

		if (a != b)
			return a - b;
		if (a == 0)
			return 0;
	}
}

/* Copy a flash-resident literal into RAM. */
void ucpy(char *dst, const char *flash_str)
{
	char c;

	do {
		c = (char)pgm_read_byte(flash_str++);
		*dst++ = c;
	} while (c);
}
