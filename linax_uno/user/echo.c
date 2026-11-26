#include <string.h>
#include "unox.h"

static char arg[40];

int main(void)
{
	sys_argv(arg, sizeof(arg));
	sys_write(1, arg, (int)strlen(arg));
	uputs("\n");
	return 0;
}
