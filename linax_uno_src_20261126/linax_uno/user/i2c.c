/* i2c: probe and transfer on the I2C bus (A4 = SDA,
 * A5 = SCL).
 * usage: i2c scan
 *        i2c w <addr> <byte> [byte...]
 *        i2c r <addr> <count>
 * Numbers are decimal, or hex with a 0x prefix. Read
 * data is printed as lowercase hex bytes. */
#include "unox.h"

static char args[44];
static char tok[12];
static char buf[16];

static int next_tok(int i, char *t, int max)
{
	int n = 0;

	while (args[i] == ' ')
		i++;
	while (args[i] && args[i] != ' ' && n < max - 1)
		t[n++] = args[i++];
	t[n] = 0;
	return i;
}

static int parse_num(const char *t)
{
	int v = 0, hex = 0;

	if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
		hex = 1;
		t += 2;
	}
	if (!*t)
		return -1;
	while (*t) {
		int d;

		if (*t >= '0' && *t <= '9')
			d = *t - '0';
		else if (hex && *t >= 'a' && *t <= 'f')
			d = *t - 'a' + 10;
		else if (hex && *t >= 'A' && *t <= 'F')
			d = *t - 'A' + 10;
		else
			return -1;
		v = v * (hex ? 16 : 10) + d;
		t++;
	}
	return v;
}

static void putc1(char c)
{
	sys_write(1, &c, 1);
}

static void putx2(int v)
{
	int hi = (v >> 4) & 15, lo = v & 15;

	putc1((char)(hi < 10 ? '0' + hi : 'a' + hi - 10));
	putc1((char)(lo < 10 ? '0' + lo : 'a' + lo - 10));
}

static int usage(void)
{
	uputs("usage: i2c scan | i2c w <addr> <byte>... | i2c r <addr> <n>\n");
	return 1;
}

static int no_ack(void)
{
	uputs("i2c: no ack\n");
	return 1;
}

int main(void)
{
	int i, addr, n, b;

	sys_argv(args, sizeof(args));
	i = next_tok(0, tok, sizeof(tok));
	if (ucmp(tok, "scan") == 0) {
		int found = 0;

		uputs("i2c scan:");
		for (addr = 8; addr < 0x78; addr++) {
			if (sys_i2c(addr, buf, 0, buf, 0) == 0) {
				uputs(" 0x");
				putx2(addr);
				found++;
			}
		}
		if (!found)
			uputs(" none");
		uputs("\n");
		return 0;
	}
	if (ucmp(tok, "w") == 0) {
		i = next_tok(i, tok, sizeof(tok));
		addr = parse_num(tok);
		n = 0;
		for (;;) {
			i = next_tok(i, tok, sizeof(tok));
			if (!tok[0] || n >= 16)
				break;
			b = parse_num(tok);
			if (b < 0 || b > 255)
				return usage();
			buf[n++] = (char)b;
		}
		if (addr < 0 || addr > 0x7F || n == 0)
			return usage();
		if (sys_i2c(addr, buf, n, buf, 0) < 0)
			return no_ack();
		return 0;
	}
	if (ucmp(tok, "r") == 0) {
		i = next_tok(i, tok, sizeof(tok));
		addr = parse_num(tok);
		i = next_tok(i, tok, sizeof(tok));
		n = parse_num(tok);
		if (addr < 0 || addr > 0x7F || n < 1 || n > 16)
			return usage();
		if (sys_i2c(addr, buf, 0, buf, n) < 0)
			return no_ack();
		for (i = 0; i < n; i++) {
			if (i)
				putc1(' ');
			putx2((unsigned char)buf[i]);
		}
		putc1('\n');
		return 0;
	}
	return usage();
}
