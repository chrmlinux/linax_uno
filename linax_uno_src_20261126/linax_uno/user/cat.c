#include <string.h>
#include "unox.h"

static char arg[40];

int main(void)
{
	char buf[24];
	int fd, n;

	sys_argv(arg, sizeof(arg));
	if (arg[0] == 0) {
		uputs("cat: no file\n");
		return 1;
	}
	fd = sys_open(arg);
	if (fd < 0) {
		uputs("cat: cannot open ");
		sys_write(1, arg, (int)strlen(arg));
		uputs("\n");
		return 1;
	}
	while ((n = sys_read(fd, buf, sizeof(buf))) > 0)
		sys_write(1, buf, n);
	sys_close(fd);
	return 0;
}
