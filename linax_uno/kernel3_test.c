/* unox M3 kernel: romfs, file syscalls, argv, shell as PID1.
 *
 * PID0 = boot/idle (always runnable). PID1 = init (a kernel
 * thread in M2; the user shell in M3). PID2/PID3 = child slots.
 * User processes share one 256-byte data window (uwindow); each
 * user pid has a save image (usave) swapped in sched_choose().
 * Syscalls enter through the gate at flash 0x2000 and run in the
 * caller's context; blocking handlers switch via ctx_switch().
 */
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <stdint.h>
#include <string.h>

static void early_init(void) __attribute__((naked, section(".init3"), used));
static void early_init(void)
{
    MCUSR = 0;
    wdt_disable();
}

#define NPROC 3
#define FRAME 35
#define SLICE_TICKS 10
#define UWIN 256

enum { FREE = 0, RUNNABLE, BLOCKED, SLEEPING, ZOMBIE };

uint16_t proc_sp[NPROC];
uint8_t cur_pid;
static uint8_t pstate[NPROC];
static uint8_t pparent[NPROC];
static uint8_t puser[NPROC];
static uint8_t pwait[NPROC];
static int pexit[NPROC];
static uint16_t pwake[NPROC];
static uint8_t slice;
volatile uint16_t tick;

uint8_t uwindow[UWIN];
static uint8_t usave[NPROC - 1][UWIN];
static uint8_t ustack[NPROC - 1][192];
static uint8_t window_owner;		/* pid or 0 */

extern void ctx_switch(void);

/* ---- scheduler ---- */

static void choose_and_swap(void)
{
    uint8_t n = cur_pid;

    do {
	n = (uint8_t)((n + 1) % NPROC);
    } while (pstate[n] != RUNNABLE);
    if (n == cur_pid)
	return;
    cur_pid = n;
    if (puser[n]) {
	if (window_owner >= 1 && window_owner != n)
	    memcpy(usave[window_owner - 1], uwindow, UWIN);
	if (window_owner != n) {
	    memcpy(uwindow, usave[n - 1], UWIN);
	    window_owner = n;
	}
    }
}

void sched_tick(void)
{
    uint8_t i;

    tick++;
    for (i = 1; i < NPROC; i++)
	if (pstate[i] == SLEEPING &&
	    (int16_t)(tick - pwake[i]) >= 0)
	    pstate[i] = RUNNABLE;
    if (++slice >= SLICE_TICKS) {
	slice = 0;
	choose_and_swap();
    }
}

void sched_switch(void)
{
    slice = 0;
    choose_and_swap();
}

/* Fabricate the initial frame (see switch.S for the layout:
 * 35 bytes at SP+1..SP+35; entry is a word address). */
static void proc_create(uint8_t pid, uint16_t entry_word,
			uint8_t *stack_top)
{
    uint8_t *sp = stack_top - (FRAME + 1);

    memset(sp + 1, 0, FRAME);
    sp[32] = (uint8_t)(1 << 7);
    sp[34] = (uint8_t)(entry_word >> 8);
    sp[35] = (uint8_t)(entry_word & 0xFF);
    proc_sp[pid] = (uint16_t)(uintptr_t)sp;
}

/* ---- UART ---- */

#define RXN 32
static volatile uint8_t rxbuf[RXN];
static volatile uint8_t rxhead, rxtail, rxcount;
static int8_t read_waiter = -1;

static void rx_inject(uint8_t c)
{
    if (rxcount < RXN) {
	rxbuf[rxhead] = c;
	rxhead = (uint8_t)((rxhead + 1) % RXN);
	rxcount++;
    }
    if (read_waiter >= 0) {
	pstate[read_waiter] = RUNNABLE;
	read_waiter = -1;
    }
}

ISR(USART_RX_vect)
{
    rx_inject(UDR0);
}

/* TEST HARNESS ONLY: feed a scripted session into the RX ring.
 * '\x01' in the script means "wait 300 ticks, feed nothing". */
static const char test_script[] PROGMEM =
    "help\n"
    "ls\n"
    "ls /bin\n"
    "cat /readme\n"
    "uname\n"
    "ps\n"
    "echo hello unox\n"
    "cd /etc\n"
    "pwd\n"
    "ls\n"
    "cat hostname\n"
    "cd /\n"
    "badcmd\n"
    "exit\n"
    "\x01\x01"
    "uname\n";
static uint16_t test_pos;
static uint16_t test_next;

static void test_feed(void)
{
    char c;

    if (test_pos >= sizeof(test_script) - 1)
	return;
    if ((int16_t)(tick - test_next) < 0 || rxcount >= 16)
	return;
    c = (char)pgm_read_byte(test_script + test_pos);
    test_pos++;
    if (c == 1) {
	test_next = (uint16_t)(tick + 300);
	return;
    }
    rx_inject((uint8_t)c);
    test_next = (uint16_t)(tick + (c == '\n' ? 60 : 2));
}

static void uart_init(void)
{
    UBRR0H = 0;
    UBRR0L = 16;
    UCSR0A = (1 << U2X0);
    UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

static void uart_putc(char c)
{
    while (!(UCSR0A & (1 << UDRE0)))
	;
    UDR0 = (uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s)
	uart_putc(*s++);
}

static void uart_putd(int v)
{
    char buf[8];
    int i = 0;
    unsigned int u;

    if (v == 0) {
	uart_putc('0');
	return;
    }
    if (v < 0) {
	uart_putc('-');
	u = (unsigned int)(-v);
    } else {
	u = (unsigned int)v;
    }
    while (u) {
	buf[i++] = (char)('0' + u % 10);
	u /= 10;
    }
    while (i)
	uart_putc(buf[--i]);
}

/* ---- romfs ----
 * Image layout (built by build_m3.py), base = romfs_img:
 *   header: u16 magic 'UR', u16 count
 *   entry (24 bytes): name[16], u8 type (1=file, 2=dir, 3=prog),
 *   u8 pad, u16 off (from base), u16 size, u16 reserved.
 * Program files hold the usual 16-byte header + binary. */

extern const uint8_t romfs_img[];

#define ENT_SIZE 24
#define FT_FILE 1
#define FT_DIR 2
#define FT_PROG 3

static uint16_t romfs_count(void)
{
    return pgm_read_word(romfs_img + 2);
}

static const uint8_t *ent_ptr(int idx)
{
    return romfs_img + 4 + idx * ENT_SIZE;
}

static uint8_t ent_type(const uint8_t *e)
{
    return pgm_read_byte(e + 16);
}

static uint16_t ent_off(const uint8_t *e)
{
    return pgm_read_word(e + 18);
}

static uint16_t ent_size(const uint8_t *e)
{
    return pgm_read_word(e + 20);
}

static int romfs_find(const char *path)
{
    uint16_t i, n = romfs_count();

    for (i = 0; i < n; i++) {
	const uint8_t *e = ent_ptr(i);
	uint8_t j = 0;

	for (;;) {
	    char a = path[j];
	    char b = (char)pgm_read_byte(e + j);

	    if (a != b)
		break;
	    if (a == 0)
		return (int)i;
	    j++;
	    if (j >= 16)
		break;
	}
    }
    return -1;
}

/* ---- file descriptors ---- */

#define NFDS 4

static struct kfd {
    uint8_t used;
    uint8_t pid;
    int8_t ent;
    uint16_t pos;
} fds[NFDS];

int sys_open(const char *path)
{
    int idx = romfs_find(path);
    const uint8_t *e;
    uint8_t i;

    if (idx < 0)
	return -1;
    e = ent_ptr(idx);
    if (ent_type(e) == FT_DIR)
	return -1;
    for (i = 0; i < NFDS; i++) {
	if (!fds[i].used) {
	    fds[i].used = 1;
	    fds[i].pid = cur_pid;
	    fds[i].ent = (int8_t)idx;
	    fds[i].pos = 0;
	    return (int)i + 3;
	}
    }
    return -1;
}

int sys_close(int fd)
{
    if (fd < 3 || fd >= 3 + NFDS || !fds[fd - 3].used)
	return -1;
    fds[fd - 3].used = 0;
    return 0;
}

int sys_readdir(int idx, char *name, int *type, int *size)
{
    const uint8_t *e;
    uint8_t j;

    if (idx < 0 || idx >= (int)romfs_count())
	return 0;
    e = ent_ptr(idx);
    for (j = 0; j < 16; j++)
	name[j] = (char)pgm_read_byte(e + j);
    name[16] = 0;
    *type = ent_type(e);
    *size = (int)ent_size(e);
    return 1;
}

/* ---- exec ---- */

#define HDR_MAGIC 0x5855	/* 'UX' */

static char pargv[NPROC][40];

static int kexec_img(const uint8_t *img)
{
    uint16_t entry, data_off, data_size, bss_size;
    uint8_t pid;
    uint16_t i;

    if (pgm_read_word(img + 0) != HDR_MAGIC)
	return -1;
    entry = pgm_read_word(img + 2);
    data_off = pgm_read_word(img + 4);
    data_size = pgm_read_word(img + 6);
    bss_size = pgm_read_word(img + 8);
    if (data_size + bss_size > UWIN)
	return -1;
    for (pid = 1; pid < NPROC; pid++)
	if (pstate[pid] == FREE)
	    break;
    if (pid == NPROC)
	return -1;
    if (window_owner == pid)
	window_owner = 0;	/* stale image of a previous incarnation */
    for (i = 0; i < data_size; i++)
	usave[pid - 1][i] = pgm_read_byte(img + data_off + i);
    memset(usave[pid - 1] + data_size, 0, bss_size);
    puser[pid] = 1;
    pparent[pid] = cur_pid;
    pwait[pid] = 0;
    pstate[pid] = RUNNABLE;
    proc_create(pid, entry, ustack[pid - 1] + sizeof(ustack[0]));
    return pid;
}

int sys_exec(const char *path, const char *args)
{
    int idx = romfs_find(path);
    const uint8_t *e;
    int pid, i;

    if (idx < 0)
	return -1;
    e = ent_ptr(idx);
    if (ent_type(e) != FT_PROG)
	return -1;
    pid = kexec_img(romfs_img + ent_off(e));
    if (pid < 0)
	return -1;
    i = 0;
    if (args) {
	while (args[i] && i < (int)sizeof(pargv[0]) - 1) {
	    pargv[pid][i] = args[i];
	    i++;
	}
    }
    pargv[pid][i] = 0;
    return pid;
}

int sys_argv(char *buf, int max)
{
    int i = 0;

    if (max <= 0)
	return 0;
    while (pargv[cur_pid][i] && i < max - 1) {
	buf[i] = pargv[cur_pid][i];
	i++;
    }
    buf[i] = 0;
    return i;
}

int sys_tasks(char *buf, int max)
{
    static const char stch[] = "FRBSZ";
    int n = 0;
    uint8_t pid;

    for (pid = 0; pid < NPROC && n < max - 8; pid++) {
	if (pstate[pid] == FREE && pid != 0)
	    continue;
	buf[n++] = (char)('0' + pid);
	buf[n++] = ' ';
	buf[n++] = stch[pstate[pid]];
	buf[n++] = ' ';
	buf[n++] = (char)('0' + pparent[pid]);
	buf[n++] = '\n';
    }
    if (n < max)
	buf[n] = 0;
    return n;
}

/* Reap one zombie child of the caller; block while children
 * live. Returns the child pid, or -1 when childless. */
int sys_wait(int *status)
{
    for (;;) {
	uint8_t pid;
	uint8_t any = 0;

	for (pid = 1; pid < NPROC; pid++) {
	    if (pparent[pid] == cur_pid && pstate[pid] != FREE) {
		any = 1;
		if (pstate[pid] == ZOMBIE) {
		    if (status)
			*status = pexit[pid];
		    pstate[pid] = FREE;
		    puser[pid] = 0;
		    return pid;
		}
	    }
	}
	if (!any)
	    return -1;
	pwait[cur_pid] = 1;
	pstate[cur_pid] = BLOCKED;
	ctx_switch();
	pwait[cur_pid] = 0;
    }
}

/* ---- syscall handlers (gate entries) ---- */

int sys_exit(int code)
{
    uint8_t p = pparent[cur_pid];
    uint8_t i;

    for (i = 0; i < NFDS; i++)
	if (fds[i].used && fds[i].pid == cur_pid)
	    fds[i].used = 0;
    pexit[cur_pid] = code;
    pstate[cur_pid] = ZOMBIE;
    if (p < NPROC && pstate[p] == BLOCKED && pwait[p])
	pstate[p] = RUNNABLE;
    ctx_switch();
    for (;;)
	;
}

int sys_write(int fd, const char *buf, int n)
{
    int i;

    if (fd != 1 && fd != 2)
	return -1;
    for (i = 0; i < n; i++) {
	if (buf[i] == '\n')
	    uart_putc('\r');
	uart_putc(buf[i]);
    }
    return n;
}

int sys_read(int fd, char *buf, int n)
{
    int i;

    if (n <= 0)
	return -1;
    if (fd >= 3) {
	struct kfd *f;
	const uint8_t *e;
	uint16_t size;
	int got = 0;

	if (fd >= 3 + NFDS)
	    return -1;
	f = &fds[fd - 3];
	if (!f->used || f->pid != cur_pid)
	    return -1;
	e = ent_ptr(f->ent);
	size = ent_size(e);
	while (got < n && f->pos < size) {
	    buf[got++] = (char)pgm_read_byte(romfs_img +
					     ent_off(e) + f->pos);
	    f->pos++;
	}
	return got;
    }
    if (fd != 0)
	return -1;
    for (;;) {
	uint8_t s = SREG;

	cli();
	if (rxcount > 0) {
	    int got = 0;

	    while (got < n && rxcount > 0) {
		buf[got++] = (char)rxbuf[rxtail];
		rxtail = (uint8_t)((rxtail + 1) % RXN);
		rxcount--;
	    }
	    i = got;
	    SREG = s;
	    return i;
	}
	read_waiter = (int8_t)cur_pid;
	pstate[cur_pid] = BLOCKED;
	SREG = s;
	ctx_switch();
    }
}

int sys_yield(void)
{
    ctx_switch();
    return 0;
}

int sys_getpid(void)
{
    return cur_pid;
}

int sys_sleep(unsigned int ticks_)
{
    if (ticks_ == 0) {
	ctx_switch();
	return 0;
    }
    pwake[cur_pid] = (uint16_t)(tick + ticks_);
    pstate[cur_pid] = SLEEPING;
    ctx_switch();
    return 0;
}

int sys_unimpl(void)
{
    return -1;
}

/* ---- boot: PID0 execs /bin/sh as PID1, then idles ---- */

static void exec_sh(void)
{
    int idx = romfs_find("/bin/sh");

    if (idx >= 0)
	kexec_img(romfs_img + ent_off(ent_ptr(idx)));
}

int main(void)
{
    uart_init();
    uart_puts("\r\nunox 0.3 (ATmega328P)\r\n");

    pstate[0] = RUNNABLE;
    cur_pid = 0;
    exec_sh();

    TCCR1A = 0;
    TCCR1B = (1 << WGM12) | (1 << CS11) | (1 << CS10);
    OCR1A = 2499;
    TIMSK1 = (1 << OCIE1A);
    sei();

    for (;;) {
	test_feed();
	/* init's other job: if the shell exits, restart it */
	if (pstate[1] == ZOMBIE) {
	    pstate[1] = FREE;
	    puser[1] = 0;
	    exec_sh();
	}
    }
}
