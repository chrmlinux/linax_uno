#include <string.h>
#include "unox.h"

static char arg[40];
static char name[17];

static const char *base(const char *p)
{
	const char *b = p;

	while (*p) {
		if (*p == '/')
			b = p + 1;
		p++;
	}
	return b;
}

static void parent_of(const char *p, char *out)
{
	int i = 0, last = -1;

	while (p[i]) {
		if (p[i] == '/')
			last = i;
		i++;
	}
	if (last <= 0) {
		out[0] = '/';
		out[1] = 0;
	} else {
		memcpy(out, p, last);
		out[last] = 0;
	}
}

int main(void)
{
	int type, size, i;
	char par[32];
	int found_file = 0;

	sys_argv(arg, sizeof(arg));
	if (arg[0] == 0) {
		arg[0] = '/';
		arg[1] = 0;
	}

	/* a plain file is listed alone */
	for (i = 0; sys_readdir(i, name, &type, &size); i++) {
		if (strcmp(name, arg) == 0 && type != 2) {
			sys_write(1, name, (int)strlen(name));
			uputs("  ");
			uputd(size);
			uputs("\n");
			found_file = 1;
		}
	}
	if (found_file)
		return 0;

	for (i = 0; sys_readdir(i, name, &type, &size); i++) {
		const char *b;

		if (name[0] == '/' && name[1] == 0)
			continue;
		parent_of(name, par);
		if (strcmp(par, arg) != 0)
			continue;
		b = base(name);
		sys_write(1, b, (int)strlen(b));
		if (type == 2)
			uputs("/");
		else {
			uputs("  ");
			uputd(size);
		}
		uputs("\n");
	}
	return 0;
}
