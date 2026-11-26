/* linaX(uno) cc: a tiny C-subset compiler.
 * Reads source from a /tmp file (streamed, one char at a
 * time), emits stack-VM bytecode into a window buffer, and
 * writes a 'UB' executable: cc /tmp/a.c -> /tmp/a.out.
 *
 * Language: variables a-z (int16), decimal literals, string
 * literals in print, + - * / %, == != < > <= >=, unary -,
 * assignment, print expr; / print "str";, if/else, while,
 * { } blocks, wait ms; and pin p, v; statements.
 * Keywords: print if else while wait pin. */
#include <string.h>
#include "unox.h"

/* bytecode ops (shared with vm.c) */
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

#define BC_MAX 96

static unsigned char bc[BC_MAX];
static int bc_len;
static int err;

static void emit(int b)
{
	if (bc_len < BC_MAX)
		bc[bc_len++] = (unsigned char)b;
	else
		err = 1;
}

static void emit16(int v)
{
	emit(v & 0xFF);
	emit((v >> 8) & 0xFF);
}

static void patch16(int pos, int v)
{
	bc[pos] = (unsigned char)(v & 0xFF);
	bc[pos + 1] = (unsigned char)((v >> 8) & 0xFF);
}

/* ---- lexer: stream the source, one lookahead char ---- */

static int infd;
static int pb = -2;		/* pushback char, -2 = none */
static int eof_seen;

static int getch(void)
{
	char c;

	if (pb != -2) {
		int r = pb;

		pb = -2;
		return r;
	}
	if (eof_seen)
		return -1;
	if (sys_read(infd, &c, 1) <= 0) {
		eof_seen = 1;
		return -1;
	}
	return (unsigned char)c;
}

#define T_EOF  0
#define T_NUM  256
#define T_VAR  257
#define T_STR  258
#define T_PRINT 259
#define T_IF   260
#define T_ELSE 261
#define T_WHILE 262
#define T_EQOP 263
#define T_NEOP 264
#define T_LEOP 265
#define T_GEOP 266
#define T_WAIT 267
#define T_PIN  268

static int tok;
static int tokval;
static int tokvar;
static char tokbuf[12];
static char strbuf[32];
static int str_len;

static void next_tok(void)
{
	int c, i;

	if (err) {
		tok = T_EOF;
		return;
	}
	do {
		c = getch();
	} while (c == ' ' || c == '\t' || c == '\r' || c == '\n');
	if (c < 0) {
		tok = T_EOF;
		return;
	}
	if (c >= '0' && c <= '9') {
		int v = 0;

		while (c >= '0' && c <= '9') {
			v = v * 10 + (c - '0');
			c = getch();
		}
		pb = c;
		tokval = v;
		tok = T_NUM;
		return;
	}
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
		i = 0;
		while ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		       (c >= '0' && c <= '9')) {
			if (i < (int)sizeof(tokbuf) - 1)
				tokbuf[i++] = (char)c;
			c = getch();
		}
		pb = c;
		tokbuf[i] = 0;
		if (i == 1 && tokbuf[0] >= 'a' && tokbuf[0] <= 'z') {
			tokvar = tokbuf[0] - 'a';
			tok = T_VAR;
		} else if (ucmp(tokbuf, "print") == 0)
			tok = T_PRINT;
		else if (ucmp(tokbuf, "if") == 0)
			tok = T_IF;
		else if (ucmp(tokbuf, "else") == 0)
			tok = T_ELSE;
		else if (ucmp(tokbuf, "while") == 0)
			tok = T_WHILE;
		else if (ucmp(tokbuf, "wait") == 0)
			tok = T_WAIT;
		else if (ucmp(tokbuf, "pin") == 0)
			tok = T_PIN;
		else
			err = 1;
		return;
	}
	if (c == '"') {
		i = 0;
		for (;;) {
			c = getch();
			if (c < 0 || c == '"')
				break;
			if (c == '\\') {
				c = getch();
				if (c == 'n')
					c = '\n';
				else if (c == 't')
					c = '\t';
			}
			if (i < (int)sizeof(strbuf) - 1)
				strbuf[i++] = (char)c;
		}
		strbuf[i] = 0;
		str_len = i;
		tok = T_STR;
		return;
	}
	if (c == '=') {
		int n = getch();

		if (n == '=') {
			tok = T_EQOP;
			return;
		}
		pb = n;
		tok = '=';
		return;
	}
	if (c == '!') {
		int n = getch();

		if (n == '=') {
			tok = T_NEOP;
			return;
		}
		err = 1;
		return;
	}
	if (c == '<') {
		int n = getch();

		if (n == '=') {
			tok = T_LEOP;
			return;
		}
		pb = n;
		tok = '<';
		return;
	}
	if (c == '>') {
		int n = getch();

		if (n == '=') {
			tok = T_GEOP;
			return;
		}
		pb = n;
		tok = '>';
		return;
	}
	tok = c;	/* + - * / % ( ) { } ; */
}

/* ---- parser: recursive descent, single pass ---- */

static void parse_expr(void);

static void expect(int t)
{
	if (tok != t)
		err = 1;
	else
		next_tok();
}

static void parse_factor(void)
{
	if (err)
		return;
	if (tok == T_NUM) {
		emit(OP_PUSH);
		emit16(tokval);
		next_tok();
	} else if (tok == T_VAR) {
		emit(OP_LOAD);
		emit(tokvar);
		next_tok();
	} else if (tok == '(') {
		next_tok();
		parse_expr();
		expect(')');
	} else if (tok == '-') {
		next_tok();
		parse_factor();
		emit(OP_NEG);
	} else {
		err = 1;
	}
}

static void parse_term(void)
{
	parse_factor();
	while (tok == '*' || tok == '/' || tok == '%') {
		int op = tok;

		next_tok();
		parse_factor();
		emit(op == '*' ? OP_MUL : op == '/' ? OP_DIV : OP_MOD);
	}
}

static void parse_add(void)
{
	parse_term();
	while (tok == '+' || tok == '-') {
		int op = tok;

		next_tok();
		parse_term();
		emit(op == '+' ? OP_ADD : OP_SUB);
	}
}

static void parse_expr(void)
{
	int op;

	parse_add();
	if (err)
		return;
	if (tok == T_EQOP || tok == T_NEOP || tok == '<' || tok == '>' ||
	    tok == T_LEOP || tok == T_GEOP) {
		op = tok;
		next_tok();
		parse_add();
		emit(op == T_EQOP ? OP_EQ : op == T_NEOP ? OP_NE :
		     op == '<' ? OP_LT : op == '>' ? OP_GT :
		     op == T_LEOP ? OP_LE : OP_GE);
	}
}

static void parse_stmt(void)
{
	if (err)
		return;
	if (tok == T_VAR) {
		int v = tokvar;

		next_tok();
		expect('=');
		parse_expr();
		emit(OP_STORE);
		emit(v);
		expect(';');
	} else if (tok == T_PRINT) {
		int i;

		next_tok();
		if (tok == T_STR) {
			emit(OP_PSTR);
			emit(str_len);
			for (i = 0; i < str_len; i++)
				emit(strbuf[i]);
			next_tok();
		} else {
			parse_expr();
			emit(OP_PRINT);
		}
		expect(';');
	} else if (tok == T_IF) {
		int jz, jmp;

		next_tok();
		expect('(');
		parse_expr();
		expect(')');
		emit(OP_JZ);
		jz = bc_len;
		emit16(0);
		parse_stmt();
		if (tok == T_ELSE) {
			emit(OP_JMP);
			jmp = bc_len;
			emit16(0);
			patch16(jz, bc_len);
			next_tok();
			parse_stmt();
			patch16(jmp, bc_len);
		} else {
			patch16(jz, bc_len);
		}
	} else if (tok == T_WHILE) {
		int start = bc_len, jz;

		next_tok();
		expect('(');
		parse_expr();
		expect(')');
		emit(OP_JZ);
		jz = bc_len;
		emit16(0);
		parse_stmt();
		emit(OP_JMP);
		emit16(start);
		patch16(jz, bc_len);
	} else if (tok == T_WAIT) {
		next_tok();
		parse_expr();
		emit(OP_WAIT);
		expect(';');
	} else if (tok == T_PIN) {
		next_tok();
		parse_expr();
		expect(',');
		parse_expr();
		emit(OP_PINW);
		expect(';');
	} else if (tok == '{') {
		next_tok();
		while (tok != '}' && tok != T_EOF && !err)
			parse_stmt();
		expect('}');
	} else if (tok == ';') {
		next_tok();
	} else {
		err = 1;
	}
}

static char srcname[24];
static char outname[24];

int main(void)
{
	int ofd, i, dot;

	sys_argv(srcname, sizeof(srcname));
	if (srcname[0] == 0) {
		uputs("usage: cc file.c\n");
		return 1;
	}
	infd = sys_open(srcname);
	if (infd < 0) {
		uputs("cc: no such file\n");
		return 1;
	}
	next_tok();
	while (tok != T_EOF && !err)
		parse_stmt();
	emit(OP_HALT);
	sys_close(infd);
	if (err) {
		uputs("cc: error\n");
		return 1;
	}
	/* output name: replace a trailing .c with .out,
	 * otherwise append .out */
	strcpy(outname, srcname);
	dot = -1;
	for (i = 0; outname[i]; i++)
		if (outname[i] == '.')
			dot = i;
	if (dot >= 0 && outname[dot + 1] == 'c' && outname[dot + 2] == 0)
		outname[dot] = 0;
	if ((int)strlen(outname) > (int)sizeof(outname) - 5) {
		uputs("cc: name too long\n");
		return 1;
	}
	ucpy(outname + strlen(outname), ".out");	/* literal is in flash */
	ofd = sys_openw(outname, 0);
	if (ofd < 0) {
		uputs("cc: cannot create out\n");
		return 1;
	}
	{
		char hdr[2];

		hdr[0] = 'U';
		hdr[1] = 'B';
		sys_write(ofd, hdr, 2);
	}
	sys_write(ofd, (char *)bc, bc_len);
	sys_close(ofd);
	return 0;
}
