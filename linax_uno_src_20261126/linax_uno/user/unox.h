/* unox userland API: syscall gate at flash word 0x1000.
 * Each entry is a JMP to the kernel handler; call the entry
 * address as a function. */
#ifndef UNOX_H
#define UNOX_H

#define GATE(n) (0x1000 + 2 * (n))

static inline void sys_exit(int code)
{
	((void (*)(int))GATE(0))(code);
	__builtin_unreachable();
}

static inline int sys_write(int fd, const char *buf, int n)
{
	return ((int (*)(int, const char *, int))GATE(1))(fd, buf, n);
}

static inline int sys_read(int fd, char *buf, int n)
{
	return ((int (*)(int, char *, int))GATE(2))(fd, buf, n);
}

static inline int sys_yield(void)
{
	return ((int (*)(void))GATE(3))();
}

static inline int sys_getpid(void)
{
	return ((int (*)(void))GATE(4))();
}

static inline int sys_sleep(unsigned int ticks)
{
	return ((int (*)(unsigned int))GATE(5))(ticks);
}

static inline int sys_exec(const char *path, const char *args)
{
	return ((int (*)(const char *, const char *))GATE(6))(path, args);
}

static inline int sys_wait(int *status)
{
	return ((int (*)(int *))GATE(7))(status);
}

static inline int sys_open(const char *path)
{
	return ((int (*)(const char *))GATE(8))(path);
}

static inline int sys_close(int fd)
{
	return ((int (*)(int))GATE(9))(fd);
}

/* Enumerate romfs entries: returns 1 and fills name (17 bytes),
 * *type (1=file, 2=dir, 3=prog) and *size, or 0 at the end. */
static inline int sys_readdir(int idx, char *name, int *type, int *size)
{
	return ((int (*)(int, char *, int *, int *))GATE(10))(idx, name, type, size);
}

/* Copy this process's argument string into buf. */
static inline int sys_argv(char *buf, int max)
{
	return ((int (*)(char *, int))GATE(11))(buf, max);
}

/* Fill buf with a process table listing (for ps). */
static inline int sys_tasks(char *buf, int max)
{
	return ((int (*)(char *, int))GATE(12))(buf, max);
}

/* Create/truncate a /tmp file for writing (append != 0 keeps
 * the content). Returns an fd usable with sys_write. */
static inline int sys_openw(const char *path, int append)
{
	return ((int (*)(const char *, int))GATE(13))(path, append);
}

/* GPIO on Arduino-numbered pins 0..19 (14..19 = A0..A5):
 * op 0 = input, 1 = input with pull-up, 2 = output,
 * 3 = write val (0/1), 4 = read (returns 0/1). */
static inline int sys_gpio(int pin, int op, int val)
{
	return ((int (*)(int, int, int))GATE(14))(pin, op, val);
}

/* I2C master transfer (A4 = SDA, A5 = SCL, 100 kHz): write
 * wlen bytes from wbuf, then read rlen bytes into rbuf.
 * wlen == rlen == 0 probes for an ACK. Returns rlen
 * (0 for write-only and probe), or -1 on error. */
static inline int sys_i2c(int addr, const char *wbuf, int wlen,
			  char *rbuf, int rlen)
{
	return ((int (*)(int, const char *, int, char *, int))GATE(15))
	    (addr, wbuf, wlen, rbuf, rlen);
}

/* String helpers: userland literals live in flash, while the
 * libc string functions and all syscall arguments need RAM
 * (data-space) strings. */
int ucmp(const char *ram, const char *flash_str);
void ucpy(char *dst, const char *flash_str);

/* Print a flash-resident string to stdout, in small RAM chunks. */
void uputs(const char *flash_str);
void uputd(int v);

#endif
