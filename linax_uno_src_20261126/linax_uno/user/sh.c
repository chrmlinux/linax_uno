/* unox shell: PID1, a real user process. Built-ins run in the
 * shell; everything else is exec'd from /bin as a child process.
 * The cwd lives in the shell's .data, i.e. in the kernel's data
 * window, so it survives child processes via the window swap. */
#include <string.h>
#include "unox.h"

static char cwd[17] = "/";
static char motd_path[] = "/etc/motd";	/* .data: a RAM string */
static char bin_prefix[] = "/bin/";	/* .data: a RAM string */
static char nl_str[] = "\n";		/* .data: a RAM string */
static char marker[8];			/* heredoc end marker */
static char line[48];
static char cmdbuf[16];
static char pathbuf[32];
static char argbuf[24];

/* command history: the last HISTSZ lines (window RAM is
 * scarce, so entries are truncated at HISTLEN chars) */
#define HISTSZ 2
#define HISTLEN 40
static char hist[HISTSZ][HISTLEN + 1];
static char hist_head;		/* next slot to overwrite */
static char hist_count;		/* valid entries, 0..HISTSZ */

static void hist_push(void)
{
	int i;

	if (hist_count > 0) {
		/* compare against the newest entry BEFORE
		 * writing: writing first would clobber the
		 * older entry in the ring */
		const char *last = hist[(hist_head + HISTSZ - 1) % HISTSZ];

		for (i = 0; ; i++) {
			char a = (i < HISTLEN) ? line[i] : 0;
			char b = (i < HISTLEN) ? last[i] : 0;

			if (a != b)
				break;
			if (a == 0)
				return;	/* duplicate of the newest */
		}
	}
	for (i = 0; i < HISTLEN && line[i]; i++)
		hist[hist_head][i] = line[i];
	hist[hist_head][i] = 0;
	hist_head = (char)((hist_head + 1) % HISTSZ);
	if (hist_count < HISTSZ)
		hist_count++;
}

static int slen(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	return n;
}

static void put_ram(const char *s)
{
	sys_write(1, s, slen(s));
}

/* Resolve arg against cwd into pathbuf, normalizing . and .. */
static void resolve(const char *arg)
{
	char tmp[40];
	int n = 0, i, o;

	if (arg[0] == '/') {
		for (i = 0; arg[i] && i < 39; i++)
			tmp[i] = arg[i];
		n = i;
	} else {
		for (i = 0; cwd[i] && i < 39; i++)
			tmp[i] = cwd[i];
		n = i;
		if (n > 1)
			tmp[n++] = '/';
		for (i = 0; arg[i] && n < 39; i++)
			tmp[n++] = arg[i];
	}
	tmp[n] = 0;

	/* normalize components */
	o = 0;
	pathbuf[o++] = '/';
	i = 0;
	while (tmp[i]) {
		int j = i, len;

		while (tmp[j] && tmp[j] != '/')
			j++;
		len = j - i;
		if (len == 0 || (len == 1 && tmp[i] == '.')) {
			/* skip */
		} else if (len == 2 && tmp[i] == '.' && tmp[i + 1] == '.') {
			while (o > 1 && pathbuf[o - 1] != '/')
				o--;
			if (o > 1)
				o--;
		} else {
			int k;

			for (k = 0; k < len && o < 30; k++)
				pathbuf[o++] = tmp[i + k];
			pathbuf[o++] = '/';
		}
		i = (tmp[j] == '/') ? j + 1 : j;
	}
	if (o > 1)
		o--;
	pathbuf[o] = 0;
}

static int is_dir(const char *path)
{
	char name[17];
	int type, size, i;

	for (i = 0; sys_readdir(i, name, &type, &size); i++)
		if (type == 2 && strcmp(name, path) == 0)
			return 1;
	return 0;
}

static void print_motd(void)
{
	char buf[24];
	int fd = sys_open(motd_path);
	int n;

	if (fd < 0)
		return;
	while ((n = sys_read(fd, buf, sizeof(buf))) > 0)
		sys_write(1, buf, n);
	sys_close(fd);
}

static void do_help(void)
{
	char name[17];
	int type, size, i;

	uputs("builtins: help cd pwd clear exit\n");
	uputs("/bin: ");
	for (i = 0; sys_readdir(i, name, &type, &size); i++) {
		if (type == 3) {
			put_ram(name + 5);
			uputs(" ");
		}
	}
	uputs("\n");
}

static char esc_read(void)
{
	char c;

	while (sys_read(0, &c, 1) <= 0)
		;
	return c;
}

static int readline(void)
{
	int n = 0;
	static char last_cr;
	static char browse_age;

	browse_age = -1;
	for (;;) {
		char c;

		if (sys_read(0, &c, 1) <= 0)
			continue;
		if (c == 27) {
			/* ESC [ A / ESC [ B: history recall */
			char c2 = esc_read(), c3 = esc_read();

			last_cr = 0;
			if (c2 == '[' && (c3 == 'A' || c3 == 'B') &&
			    hist_count > 0) {
				int age = browse_age;

				if (c3 == 'A') {
					if (age < hist_count - 1)
						age++;
				} else {
					if (age > -1)
						age--;
				}
				if (age != browse_age) {
					while (n > 0) {
						char e[3];

						e[0] = 8; e[1] = ' '; e[2] = 8;
						n--;
						sys_write(1, e, 3);
					}
					if (age >= 0) {
						const char *s = hist[(hist_head + HISTSZ - 1 - age) % HISTSZ];
						int i;

						for (i = 0; s[i]; i++) {
							line[i] = s[i];
							sys_write(1, line + i, 1);
						}
						n = i;
					}
					line[n] = 0;
					browse_age = (char)age;
				}
			}
			continue;
		}
		if (c == 3) {	/* Ctrl+C: discard the line */
			last_cr = 0;
			while (n > 0) {
				char e[3];

				e[0] = 8; e[1] = ' '; e[2] = 8;
				n--;
				sys_write(1, e, 3);
			}
			uputs("^C\n");
			line[0] = 0;
			return -1;
		}
		if (c == '\n' && last_cr) {
			last_cr = 0;
			continue;
		}
		last_cr = (c == '\r');
		if (c == '\r' || c == '\n') {
			uputs("\n");
			break;
		}
		if (c == 8 || c == 127) {
			browse_age = -1;
			if (n > 0) {
				char e[3];

				e[0] = 8; e[1] = ' '; e[2] = 8;
				n--;
				sys_write(1, e, 3);
			}
			continue;
		}
		if (c >= ' ' && c < 127 && n < (int)sizeof(line) - 1) {
			browse_age = -1;
			line[n++] = c;
			sys_write(1, &c, 1);
		}
	}
	line[n] = 0;
	return n;
}

int main(void)
{
	print_motd();
	for (;;) {
		char *p, *args;
		int i, st;

		put_ram(cwd);
		uputs("# ");
		if (readline() <= 0)
			continue;
		p = line;
		while (*p == ' ')
			p++;
		if (*p == 0)
			continue;
		hist_push();
		i = 0;
		while (p[i] && p[i] != ' ' && i < 15)
			i++;
		memcpy(cmdbuf, p, i);
		cmdbuf[i] = 0;
		args = p + i;
		while (*args == ' ')
			args++;

		if (ucmp(cmdbuf, "exit") == 0) {
			uputs("logout\n");
			return 0;
		}
		if (ucmp(cmdbuf, "help") == 0) {
			do_help();
			continue;
		}
		if (ucmp(cmdbuf, "pwd") == 0) {
			put_ram(cwd);
			uputs("\n");
			continue;
		}
		if (ucmp(cmdbuf, "clear") == 0) {
			char e[7];

			e[0] = 27; e[1] = '['; e[2] = '2'; e[3] = 'J';
			e[4] = 27; e[5] = '['; e[6] = 'H';
			sys_write(1, e, 7);
			continue;
		}
		if (ucmp(cmdbuf, "cd") == 0) {
			if (*args == 0) {
				cwd[0] = '/';
				cwd[1] = 0;
				continue;
			}
			resolve(args);
			if (is_dir(pathbuf))
				strcpy(cwd, pathbuf);
			else {
				uputs("cd: no such dir: ");
				put_ram(args);
				uputs("\n");
			}
			continue;
		}

		if (ucmp(cmdbuf, "echo") == 0) {
			/* echo TEXT > /tmp/file (or >> to append),
			 * handled by the shell itself: a child
			 * process cannot inherit an output fd.
			 * Heredoc form: echo > /tmp/file <<EOF,
			 * then lines until a line that is exactly
			 * the marker. */
			char *g = args;
			int append = 0, fd, heredoc = 0;

			{
				char *h = args, *m;
				int mi = 0;

				while (h[0] && h[1] &&
				       !(h[0] == '<' && h[1] == '<'))
					h++;
				if (h[0] == '<' && h[1] == '<') {
					m = h + 2;
					while (*m == ' ')
						m++;
					while (m[mi] && m[mi] != ' ' &&
					       mi < (int)sizeof(marker) - 1) {
						marker[mi] = m[mi];
						mi++;
					}
					marker[mi] = 0;
					if (mi > 0) {
						char *src = m + mi, *dst = h;

						heredoc = 1;
						while (*src)
							*dst++ = *src++;
						*dst = 0;
					}
				}
			}
			while (*g && *g != '>')
				g++;
			if (*g == '>') {
				char *tgt, *txt, *e;
				int tl;

				*g = 0;
				g++;
				if (*g == '>') {
					append = 1;
					g++;
				}
				e = args + slen(args);
				while (e > args && e[-1] == ' ')
					*--e = 0;
				tgt = g;
				while (*tgt == ' ')
					tgt++;
				e = tgt + slen(tgt);
				while (e > tgt && e[-1] == ' ')
					*--e = 0;
				txt = args;
				while (*txt == ' ')
					txt++;
				tl = slen(txt);
				if (tl >= 2 && txt[0] == '"' &&
				    txt[tl - 1] == '"') {
					txt[tl - 1] = 0;
					txt++;
				}
				resolve(tgt);
				fd = sys_openw(pathbuf, append);
				if (fd < 0) {
					uputs("sh: cannot write: ");
					put_ram(tgt);
					uputs("\n");
				} else if (heredoc) {
					for (;;) {
						int n;

						uputs("> ");
						n = readline();
						if (n < 0)	/* Ctrl+C aborts */
							break;
						if (strcmp(line, marker) == 0)
							break;
						sys_write(fd, line, n);
						sys_write(fd, nl_str, 1);
					}
					sys_close(fd);
				} else {
					sys_write(fd, txt, slen(txt));
					sys_write(fd, nl_str, 1);
					sys_close(fd);
				}
				continue;
			}
		}

		/* external command. Commands that take a path
		 * (cat, ls) get a cwd-resolved absolute path:
		 * the cwd is the shell's private state, so the
		 * shell resolves before exec. */
		{
			const char *exec_args = args;
			int pid;

			if (ucmp(cmdbuf, "ls") == 0 && *args == 0)
				exec_args = cwd;
			else if ((ucmp(cmdbuf, "cat") == 0 ||
				  ucmp(cmdbuf, "ls") == 0) &&
				 *args && !strchr(args, ' ') &&
				 args[0] != '/') {
				resolve(args);
				strcpy(argbuf, pathbuf);
				exec_args = argbuf;
			}
			if (strchr(cmdbuf, '/'))
				resolve(cmdbuf);
			else {
				strcpy(pathbuf, bin_prefix);
				strcat(pathbuf, cmdbuf);
			}
			pid = sys_exec(pathbuf, exec_args);
			if (pid < 0) {
				uputs("sh: not found: ");
				put_ram(cmdbuf);
				uputs("\n");
				continue;
			}
			sys_wait(&st);
			if (st != 0) {
				uputs("[exit ");
				uputd(st);
				uputs("]\n");
			}
		}
	}
}
