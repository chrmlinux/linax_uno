#include "unox.h"

static char buf[64];

int main(void)
{
	int n;

	uputs("pid st ppid\n");
	n = sys_tasks(buf, sizeof(buf));
	if (n > 0)
		sys_write(1, buf, n);
	return 0;
}
