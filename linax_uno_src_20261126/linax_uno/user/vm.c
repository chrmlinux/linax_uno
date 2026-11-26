/* linaX(uno) vm: runs 'UB' bytecode files produced by cc.
 * Normally launched by the kernel's exec when a /tmp file
 * with the 'UB' magic is executed; argv is the file path.
 * Stack machine, int16 arithmetic, variables a-z. */
#include <string.h>
#include "unox.h"

#define OP_HALT  0x00
#define OP_PUSH  0x01
#define OP_LOAD  0x02
#define OP_STORE 0x03
#define OP_ADD   0x04
#define OP_SUB   0x05
#define OP_MUL   0x06
#define OP_DIV   0x07
#define OP_MOD   0x08
#define OP_NEG   0x09
#define OP_EQ    0x0A
#define OP_NE    0x0B
#define OP_LT    0x0C
#define OP_GT    0x0D
#define OP_LE    0x0E
#define OP_GE    0x0F
#define OP_JZ    0x10
#define OP_JMP   0x11
#define OP_PRINT 0x12
#define OP_PSTR  0x13
#define OP_WAIT  0x14
#define OP_PINW  0x15

#define CODE_MAX 96
#define STK_MAX 10

static unsigned char code[CODE_MAX];
static int code_len;
static int vars[26];
static int st[STK_MAX];
static int sp;

static int push(int v)
{
	if (sp >= STK_MAX)
		return -1;
	st[sp++] = v;
	return 0;
}

static int pop(void)
{
	return sp > 0 ? st[--sp] : 0;
}

static char path[24];

int main(void)
{
	int fd, n, pc;
	char hdr[2];

	sys_argv(path, sizeof(path));
	if (path[0] == 0) {
		uputs("vm: no file\n");
		return 1;
	}
	fd = sys_open(path);
	if (fd < 0) {
		uputs("vm: no such file\n");
		return 1;
	}
	if (sys_read(fd, hdr, 2) != 2 || hdr[0] != 'U' || hdr[1] != 'B') {
		uputs("vm: bad magic\n");
		return 1;
	}
	code_len = 0;
	while ((n = sys_read(fd, (char *)code + code_len,
			     CODE_MAX - code_len)) > 0)
		code_len += n;
	sys_close(fd);
	if (code_len >= CODE_MAX) {
		uputs("vm: too big\n");
		return 1;
	}
	pc = 0;
	sp = 0;
	while (pc >= 0 && pc < code_len) {
		int op = code[pc++];
		int a, b;

		switch (op) {
		case OP_HALT:
			return 0;
		case OP_PUSH:
			a = code[pc] | (code[pc + 1] << 8);
			pc += 2;
			if (push(a) < 0)
				goto stkerr;
			break;
		case OP_LOAD:
			if (push(vars[code[pc++]]) < 0)
				goto stkerr;
			break;
		case OP_STORE:
			vars[code[pc++]] = pop();
			break;
		case OP_ADD: b = pop(); a = pop(); push(a + b); break;
		case OP_SUB: b = pop(); a = pop(); push(a - b); break;
		case OP_MUL: b = pop(); a = pop(); push(a * b); break;
		case OP_DIV:
			b = pop(); a = pop();
			if (b == 0) {
				uputs("vm: div0\n");
				return 1;
			}
			push(a / b);
			break;
		case OP_MOD:
			b = pop(); a = pop();
			if (b == 0) {
				uputs("vm: div0\n");
				return 1;
			}
			push(a % b);
			break;
		case OP_NEG: push(-pop()); break;
		case OP_EQ: b = pop(); a = pop(); push(a == b); break;
		case OP_NE: b = pop(); a = pop(); push(a != b); break;
		case OP_LT: b = pop(); a = pop(); push(a < b); break;
		case OP_GT: b = pop(); a = pop(); push(a > b); break;
		case OP_LE: b = pop(); a = pop(); push(a <= b); break;
		case OP_GE: b = pop(); a = pop(); push(a >= b); break;
		case OP_JZ:
			a = code[pc] | (code[pc + 1] << 8);
			pc += 2;
			if (pop() == 0)
				pc = a;
			break;
		case OP_JMP:
			pc = code[pc] | (code[pc + 1] << 8);
			break;
		case OP_PRINT:
			uputd(pop());
			uputs("\n");
			break;
		case OP_PSTR:
			a = code[pc++];
			sys_write(1, (char *)code + pc, a);
			pc += a;
			uputs("\n");
			break;
		case OP_WAIT:	/* operand in milliseconds */
			a = pop();
			sys_sleep((unsigned int)(a > 0 ? a / 10 : 0));
			break;
		case OP_PINW:	/* pin p, v: output mode, then write */
			b = pop(); a = pop();
			sys_gpio(a, 2, 0);
			sys_gpio(a, 3, b);
			break;
		default:
			uputs("vm: bad op\n");
			return 1;
		}
	}
	return 0;

stkerr:
	uputs("vm: stack overflow\n");
	return 1;
}
