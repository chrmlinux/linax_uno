#!/usr/bin/env python3
"""Generate user/busybox.c from the standalone applet sources.
Each applet's file-scope statics are folded into one union U
(only one applet runs per process, so the buffers overlay),
its main() becomes app_<name>(const char *args), and its
sys_argv() call becomes an argcpy() from the dispatch args.
Function-name collisions between applets are renamed."""
import re

def strip_includes(src):
    lines = src.split("\n")
    out = []
    seen_inc = False
    for ln in lines:
        if ln.startswith("#include"):
            seen_inc = True
            continue
        if seen_inc and ln.startswith("#include"):
            continue
        out.append(ln)
    return "\n".join(out)

def drop_decls(src, prefixes):
    out = []
    for ln in src.split("\n"):
        s = ln.strip()
        if any(s.startswith(p) for p in prefixes):
            continue
        out.append(ln)
    return "\n".join(out)

def rename(src, pairs):
    for name, repl in pairs:
        src = re.sub(r"\b" + re.escape(name) + r"\b", repl, src)
    return src

def transform(fname, app, decl_prefixes, renames, extra=None):
    src = open(f"user/{fname}").read()
    src = strip_includes(src)
    src = drop_decls(src, decl_prefixes)
    # renames first: the two replacements below insert the
    # identifier "args", which must not be renamed itself
    src = rename(src, renames)
    # sys_argv(buf, sizeof(buf)) -> argcpy(buf, args, sizeof(buf))
    src = re.sub(r"sys_argv\(([\w.]+), sizeof\([\w.]+\)\);",
                 r"argcpy(\1, args, (int)sizeof(\1));", src)
    src = src.replace("int main(void)",
                      f"static int app_{app}(const char *args)")
    if extra:
        src = extra(src)
    return src.strip("\n")

# ---- per-applet transforms ----

def t_ls():
    return transform("ls.c", "ls",
        ["static char arg[40];", "static char name[17];"],
        [("arg", "U.g.arg"), ("name", "U.g.name")])

def t_cat():
    return transform("cat.c", "cat",
        ["static char arg[40];"],
        [("arg", "U.g.arg")])

def t_echo():
    return transform("echo.c", "echo",
        ["static char arg[40];"],
        [("arg", "U.g.arg")])

def t_uname():
    src = transform("uname.c", "uname", [], [])
    return src.replace("linaX(uno) 0.7", "linaX(uno) 0.8")

def t_ps():
    return transform("ps.c", "ps",
        ["static char buf[64];"],
        [("buf", "U.g.psbuf")])

def t_cc():
    def extra(src):
        # pb's .data initializer (-2) is lost in the union's
        # bss; set it at entry instead
        old = "\tint ofd, i, dot;\n"
        assert src.count(old) == 1
        return src.replace(old, old + "\n\tU.cc.pb = -2;\n")
    return transform("cc.c", "cc",
        ["static unsigned char bc[BC_MAX];", "static int bc_len;",
         "static int err;", "static int infd;", "static int pb = -2;",
         "static int eof_seen;", "static int tok;", "static int tokval;",
         "static int tokvar;", "static char tokbuf[12];",
         "static char strbuf[32];", "static int str_len;",
         "static char srcname[24];", "static char outname[24];"],
        [("srcname", "U.cc.srcname"), ("outname", "U.cc.outname"),
         ("tokbuf", "U.cc.tokbuf"), ("strbuf", "U.cc.strbuf"),
         ("str_len", "U.cc.str_len"), ("tokval", "U.cc.tokval"),
         ("tokvar", "U.cc.tokvar"), ("bc_len", "U.cc.bc_len"),
         ("eof_seen", "U.cc.eof_seen"), ("infd", "U.cc.infd"),
         ("next_tok", "cc_next_tok"), ("bc", "U.cc.bc"),
         ("err", "U.cc.err"), ("pb", "U.cc.pb"), ("tok", "U.cc.tok")],
        extra)

def t_vm():
    def extra(src):
        # OP_* defines duplicate cc's; drop them
        out = [ln for ln in src.split("\n")
               if not ln.startswith("#define OP_")]
        return "\n".join(out)
    return transform("vm.c", "vm",
        ["static unsigned char code[CODE_MAX];", "static int code_len;",
         "static int vars[26];", "static int st[STK_MAX];",
         "static int sp;", "static char path[24];"],
        [("code_len", "U.vm.code_len"), ("path", "U.vm.path"),
         ("vars", "U.vm.vars"), ("code", "U.vm.code"),
         ("st", "U.vm.st"), ("sp", "U.vm.sp")],
        extra)

def t_gpio():
    return transform("gpio.c", "gpio",
        ["static char args[40];", "static char t0[12];",
         "static char t1[12];"],
        [("next_tok", "gpio_next_tok"), ("usage", "gpio_usage"),
         ("args", "U.g.arg"), ("t0", "U.g.tok"), ("t1", "U.g.tok1")])

def t_i2c():
    return transform("i2c.c", "i2c",
        ["static char args[44];", "static char tok[12];",
         "static char buf[16];"],
        [("next_tok", "i2c_next_tok"), ("usage", "i2c_usage"),
         ("args", "U.g.arg"), ("tok", "U.g.tok"), ("buf", "U.g.buf")])

def t_sleep():
    return transform("sleep.c", "sleep",
        ["static char args[16];"],
        [("args", "U.g.arg")])

HEADER = """/* linaX(uno) busybox: the /bin commands as applets of one
 * multicall image (BusyBox style). The kernel's argv
 * convention is "<name> <args>"; main dispatches on the
 * name. The romfs holds one entry per applet name, all
 * pointing at this single image.
 *
 * All applet statics live in one union: only one applet
 * runs in a process, so their buffers overlay. The data
 * window is 256 bytes; the union is sized by the largest
 * applet (cc, 206 bytes).
 *
 * Generated by gen_busybox.py from the standalone applet
 * sources (ls.c, cat.c, ...), which are kept for reference. */
#include <string.h>
#include "unox.h"

static char bb_argv[40];

static union {
\tstruct {
\t\tunsigned char bc[96];
\t\tint bc_len;
\t\tint err;
\t\tint infd;
\t\tint pb;
\t\tint eof_seen;
\t\tint tok;
\t\tint tokval;
\t\tint tokvar;
\t\tchar tokbuf[12];
\t\tchar strbuf[32];
\t\tint str_len;
\t\tchar srcname[24];
\t\tchar outname[24];
\t} cc;
\tstruct {
\t\tunsigned char code[96];
\t\tint code_len;
\t\tint vars[26];
\t\tint st[10];
\t\tint sp;
\t\tchar path[24];
\t} vm;
\tstruct {
\t\tchar arg[44];
\t\tchar name[17];
\t\tchar tok[12];
\t\tchar tok1[12];
\t\tchar buf[16];
\t\tchar psbuf[64];
\t} g;
} U;

/* Copy the applet's argument string, truncating like
 * sys_argv did. src is a RAM string. */
static void argcpy(char *dst, const char *src, int max)
{
\tint i = 0;

\twhile (src[i] && i < max - 1) {
\t\tdst[i] = src[i];
\t\ti++;
\t}
\tdst[i] = 0;
}
"""

MAIN = """
int main(void)
{
\tconst char *args;
\tchar *p;

\tsys_argv(bb_argv, sizeof(bb_argv));
\tp = bb_argv;
\twhile (*p && *p != ' ')
\t\tp++;
\tif (*p == ' ')
\t\t*p++ = 0;
\targs = p;
\tif (ucmp(bb_argv, "ls") == 0)
\t\treturn app_ls(args);
\tif (ucmp(bb_argv, "cat") == 0)
\t\treturn app_cat(args);
\tif (ucmp(bb_argv, "echo") == 0)
\t\treturn app_echo(args);
\tif (ucmp(bb_argv, "uname") == 0)
\t\treturn app_uname(args);
\tif (ucmp(bb_argv, "ps") == 0)
\t\treturn app_ps(args);
\tif (ucmp(bb_argv, "cc") == 0)
\t\treturn app_cc(args);
\tif (ucmp(bb_argv, "vm") == 0)
\t\treturn app_vm(args);
\tif (ucmp(bb_argv, "gpio") == 0)
\t\treturn app_gpio(args);
\tif (ucmp(bb_argv, "i2c") == 0)
\t\treturn app_i2c(args);
\tif (ucmp(bb_argv, "sleep") == 0)
\t\treturn app_sleep(args);
\tuputs("busybox: unknown applet\\n");
\treturn 1;
}
"""

parts = [HEADER, t_ls(), t_cat(), t_echo(), t_uname(), t_ps(),
         t_cc(), t_vm(), t_gpio(), t_i2c(), t_sleep(), MAIN]
open("user/busybox.c", "w").write("\n\n".join(parts) + "\n")
print("user/busybox.c generated")
