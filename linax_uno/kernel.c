/* unox M2 kernel: user processes, exec, syscall gate, wait/exit.
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

ISR(USART_RX_vect)
{
    uint8_t c = UDR0;

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

/* ---- program images and exec ---- */

extern const uint8_t prog1_img[];
extern const uint8_t prog2_img[];

struct prog_ent {
    const char *name;
    const uint8_t *img;
};

static const char prog1_name[] PROGMEM = "prog1";
static const char prog2_name[] PROGMEM = "prog2";

static const struct prog_ent prog_tab[] PROGMEM = {
    { prog1_name, prog1_img },
    { prog2_name, prog2_img },
};

#define HDR_MAGIC 0x5855	/* 'UX' */

/* The name argument is always a data-space (RAM) string: kernel
 * literals are RAM copies, and the shell's buffer is in the
 * window. Table names are PROGMEM. */
static const uint8_t *find_prog(const char *name)
{
    uint8_t i;

    for (i = 0; i < sizeof(prog_tab) / sizeof(prog_tab[0]); i++) {
	const char *t =
	    (const char *)pgm_read_ptr(&prog_tab[i].name);
	uint8_t j = 0;

	for (;;) {
	    char a = name[j];
	    char b = (char)pgm_read_byte(t + j);

	    if (a != b)
		break;
	    if (a == 0)
		return (const uint8_t *)
		    pgm_read_ptr(&prog_tab[i].img);
	    j++;
	}
    }
    return 0;
}

/* Create a user process from a flash image. Returns pid or -1. */
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

int sys_exec(const char *name)
{
    const uint8_t *img = find_prog(name);

    if (!img)
	return -1;
    return kexec_img(img);
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

    if (fd != 0 || n <= 0)
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

/* ---- init thread (PID1) and boot ---- */

static uint8_t kstack1[192];

static void demo_init(void)
{
    int st = -1;
    int w;

    uart_puts("init: exec prog1 -> ");
    uart_putd(sys_exec("prog1"));
    uart_puts("\r\n");
    w = sys_wait(&st);
    uart_puts("init: reaped pid ");
    uart_putd(w);
    uart_puts(", prog1 exit ");
    uart_putd(st);
    uart_puts("\r\n");

    uart_puts("init: exec prog2 -> ");
    uart_putd(sys_exec("prog2"));
    uart_puts("\r\n");
    w = sys_wait(&st);
    uart_puts("init: reaped pid ");
    uart_putd(w);
    uart_puts(", prog2 exit ");
    uart_putd(st);
    uart_puts("\r\n");

    uart_puts("unox M2 done\r\n");
    for (;;)
	ctx_switch();
}

int main(void)
{
    uart_init();
    uart_puts("\r\nunox M2: user processes\r\n");

    pstate[0] = RUNNABLE;
    cur_pid = 0;
    proc_create(1, (uint16_t)(uintptr_t)demo_init,
		kstack1 + sizeof(kstack1));
    pstate[1] = RUNNABLE;
    pparent[1] = 0;

    TCCR1A = 0;
    TCCR1B = (1 << WGM12) | (1 << CS11) | (1 << CS10);
    OCR1A = 2499;
    TIMSK1 = (1 << OCIE1A);
    sei();

    for (;;)
	;			/* PID0 idle */
}
