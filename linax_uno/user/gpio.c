/* gpio: drive one Arduino-numbered pin.
 * usage: gpio <pin> in|inpull|out|hi|lo|read
 * <pin> is 0..19, or A0..A5 for pins 14..19. hi and lo
 * switch the pin to output first. read prints 0 or 1. */
#include "unox.h"

static char args[40];
static char t0[12];
static char t1[12];

static int next_tok(int i, char *tok, int max)
{
	int n = 0;

	while (args[i] == ' ')
		i++;
	while (args[i] && args[i] != ' ' && n < max - 1)
		tok[n++] = args[i++];
	tok[n] = 0;
	return i;
}

static int parse_pin(const char *t)
{
	int v = 0;

	if (t[0] == 'A' || t[0] == 'a') {
		if (t[1] < '0' || t[1] > '5' || t[2])
			return -1;
		return 14 + (t[1] - '0');
	}
	if (!*t)
		return -1;
	while (*t) {
		if (*t < '0' || *t > '9')
			return -1;
		v = v * 10 + (*t++ - '0');
	}
	return v;
}

static int usage(void)
{
	uputs("usage: gpio <pin> in|inpull|out|hi|lo|read\n");
	return 1;
}

int main(void)
{
	int i, pin, op, val = 0, r;

	sys_argv(args, sizeof(args));
	i = next_tok(0, t0, sizeof(t0));
	next_tok(i, t1, sizeof(t1));
	pin = parse_pin(t0);
	if (pin < 0 || !t1[0])
		return usage();
	if (ucmp(t1, "in") == 0)
		op = 0;
	else if (ucmp(t1, "inpull") == 0)
		op = 1;
	else if (ucmp(t1, "out") == 0)
		op = 2;
	else if (ucmp(t1, "hi") == 0) {
		op = 3;
		val = 1;
	} else if (ucmp(t1, "lo") == 0)
		op = 3;
	else if (ucmp(t1, "read") == 0)
		op = 4;
	else
		return usage();
	if (op == 3 && sys_gpio(pin, 2, 0) < 0) {
		uputs("gpio: error\n");
		return 1;
	}
	r = sys_gpio(pin, op, val);
	if (r < 0) {
		uputs("gpio: error\n");
		return 1;
	}
	if (op == 4) {
		uputd(r);
		uputs("\n");
	}
	return 0;
}
