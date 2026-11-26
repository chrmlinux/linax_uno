/* sleep: wait for the given number of 10 ms ticks.
 * usage: sleep <ticks>   (sleep 100 = 1 second)
 * The kernel's sys_sleep compares wake times with a
 * 16-bit signed difference, so one call can sleep at
 * most 32767 ticks; long waits are split into chunks. */
#include "unox.h"

static char args[16];

int main(void)
{
	int i = 0, any = 0;
	unsigned long v = 0;

	sys_argv(args, sizeof(args));
	while (args[i] == ' ')
		i++;
	while (args[i] >= '0' && args[i] <= '9') {
		v = v * 10 + (unsigned long)(args[i++] - '0');
		any = 1;
	}
	if (!any) {
		uputs("usage: sleep <ticks> (1 tick = 10 ms)\n");
		return 1;
	}
	while (v > 0) {
		unsigned int chunk = v > 30000UL ? 30000 : (unsigned int)v;

		sys_sleep(chunk);
		v -= chunk;
	}
	return 0;
}
