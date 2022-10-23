// very simple tty interface, inspired by and in part nicked from linenoise
// https://github.com/antirez/linenoise
//

#ifndef RAWTTY_H
#define RAWTTY_H

#include <stdint.h>

// not sure how portable some of these are...
#define K_LEFT 0x5b44
#define K_RIGHT 0x5b43
#define K_UP 0x5b41
#define K_DOWN 0x5b42
#define K_DEL 0x5b337e
#define K_BACKSP 0x7f
#define K_CTRLA 0x01
#define K_CTRLC 0x03
#define K_CTRLD 0x04
#define K_CTRLE 0x05
#define K_CR 0x0d
#define K_ESCAPE 0x1b

/* initialize tty lib, enter raw mode. call before use */
extern int tty_init();
/* leave raw mode, cleanup. Call before program exit */
extern void tty_deinit();
/* updates the tty libs idea of the terminal size. Is called by tty_init(),
 * call when the terminal is resized */
extern int tty_updateSize();
/* printf() a string to the raw terminal */
extern int tty_printf(const char *fmt, ...);
/* print a string to the raw terminal */
extern int tty_print(const char *str);
/* output a single char to the terminal */
extern int tty_writeByte(char ch);
/* EL (Erase Line)
 *    Sequence: ESC [ n K
 *    Effect: if n is 0 or missing, clear from cursor to end of line
 *    Effect: if n is 1, clear from beginning of line to cursor
 *    Effect: if n is 2, clear entire line */
extern int tty_eraseLine(uint32_t n);
/* CUF (CUrsor Forward)
 *    Sequence: ESC [ n C
 *    Effect: moves cursor forward n chars */
extern int tty_cursorFwd(uint32_t n);
/* CUB (CUrsor Backward)
 *    Sequence: ESC [ n D
 *    Effect: moves cursor backward n chars */
extern int tty_cursorBack(uint32_t n);
/* CUU (Cursor Up)
 *    Sequence: ESC [ n A
 *    Effect: moves cursor up of n chars. */
extern int tty_cursorUp(uint32_t n);
/* CUD (Cursor Down)
 *    Sequence: ESC [ n B
 *    Effect: moves cursor down of n chars. */
extern int tty_cursorDown(uint32_t n);
/* CUP (Cursor position)
 *    Sequence: ESC [ H
 *    Effect: moves the cursor to upper left */
extern int tty_cursorHome();
/* ED (Erase display)
 *    Sequence: ESC [ 2 J
 *    Effect: clear the whole screen */
extern int tty_clearScreen();
/* output CR-NL to the tty */
extern int tty_newline();
/* return 1 if a byte can be read from the terminal, 0 otherwise */
extern int tty_canRead();
/* read one byte from the terminal */
extern int tty_readByte();
/* read a key from the terminal. Key consts are listed above. If the
 * result is <256, then the key is a single char */
extern int32_t tty_readKey();
/* if a key can be read, return it (like readKey()), otherwise return 0 */
extern int32_t tty_checkKey();
/* read one line from the terminal into a buffer, with simple line
 * editing. This is really simple, lines can not be longer than the
 * terminal is wide. */
extern const char* tty_readLine(char *buf, int buflen);

#ifdef RAWTTY_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>

static int tty = -1;
static struct termios tios;
static int16_t rows = 0;
static int16_t cols = 0;

// nicked from linenoise
static int tty_makeraw()
{
    // input modes: no break, no CR to NL, no parity check, no strip char,
    // no start/stop output control.
    tios.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    // output modes - disable post processing
    tios.c_oflag &= ~(OPOST);
    // control modes - set 8 bit chars
    tios.c_cflag |= (CS8);
    // local modes - choing off, canonical off, no extended functions,
    // no signal chars (^Z,^C)
    tios.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    // control chars - set return condition: min number of bytes and timer.
    // We want read to return every single byte, without timeout.
    tios.c_cc[VMIN] = 1;
    tios.c_cc[VTIME] = 0; // 1 byte, no timer
    return tcsetattr(tty, TCSAFLUSH, &tios);
}

int tty_updateSize()
{
    struct winsize ws; 
    int ok = ioctl(tty, TIOCGWINSZ, &ws);
    if (ok == 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    return ok;
}

void tty_deinit()
{
    if (tty == -1) return;
    tcsetattr(tty, TCSAFLUSH, &tios);
    close(tty);
    tty = -1;
}

int tty_init()
{
    if (tty == -1) tty = open("/dev/tty", O_RDWR);
    if (tty == -1) return 0;
    if (tcgetattr(tty, &tios) == -1) goto error;
    if (tty_makeraw() == -1) goto error0;
    if (tty_updateSize() == -1) goto error0;
    return 0;
    error0: tcsetattr(tty, TCSAFLUSH, &tios);
    error: close(tty);
    tty = -1;
    return -1;
}

int tty_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap) + 1;
    va_end(ap);
    char *buf = malloc(len);
    va_start(ap, fmt);
    vsnprintf(buf, len, fmt, ap);
    va_end(ap);
    int res = 0;
    if (write(tty, buf, len) != len) res = -1;
    free(buf);
    return res;
}

int tty_print(const char *str)
{
    int len = strlen(str);
    if (write(tty, str, len) != len) return -1;
    return 0;
}

int tty_eraseLine(uint32_t n)
{
    return tty_printf("\x1b[%dK", n % 3);
}

int tty_cursorFwd(uint32_t n)
{
    return tty_printf("\x1b[%dC", n);
}

int tty_cursorBack(uint32_t n)
{
    return tty_printf("\x1b[%dD", n);
}

int tty_cursorUp(uint32_t n)
{
    return tty_printf("\x1b[%dA", n);
}

int tty_cursorDown(uint32_t n)
{
    return tty_printf("\x1b[%dB", n);
}

int tty_cursorHome()
{
    return tty_print("\x1b[H");
}

int tty_clearScreen()
{
    return tty_print("\x1b[2J");
}

int tty_newline()
{
    return tty_print("\r\n");
}

int tty_writeByte(char ch)
{
    if (write(tty, &ch, 1) != 1) return -1;
    return 0;
}

int tty_canRead()
{
    struct pollfd pollfds[1] = {
        { fd: tty, events: POLLIN, revents: 0 },
    };

    // don't wait for the fd to become ready...
    if (0 < poll(&pollfds[0], 1, 0)) {
        if (pollfds[0].revents & POLLIN != 0) {
            return 1;
        }
    }
    return 0;
}

int tty_readByte()
{
    char buf[1];
    if (read(tty, &buf, 1) < 1) return -1;
    return buf[0];
}

int32_t tty_readKey()
{
    int32_t r = tty_readByte();
    if (r < 0) return 0;
    if (r == 0x1b) {
        int32_t which = 0;
        while (tty_canRead() > 0) {
            which = (which << 8) | tty_readByte();
        }
        if (which > 0) r = which;
    }
    return r;
}

int32_t tty_checkKey()
{
    if (tty_canRead()) return tty_readKey();
    return 0;
}

static int tty_rlClearline(uint32_t cursor, uint32_t len)
{
    tty_cursorBack(cursor);
    //uint32_t cur;
    //for (cur = 0; cur < len; cur += 1) tty_writeByte(' ');
    //tty_cursorBack(len);
    tty_eraseLine(0);
    return 0;
}

static int tty_rlDelChar(char *buf, int buflen, uint32_t cursor, uint32_t len)
{
    if (len > buflen) return -1;
    uint32_t cur;
    for (cur = 0; cur < len - 1; cur += 1) {
        buf[cur] = buf[cur + 1];
        tty_writeByte(buf[cur]);
    }
    tty_writeByte(' ');
    tty_cursorBack(len - cursor);
    return 0;
}

static int tty_rlInsChar(char *buf, int buflen, uint32_t cursor, uint32_t len)
{
    if (len == buflen) return -1;
    if (cursor < len) {
        uint32_t cur;
        for (cur = len + 1; cur > cursor; cur -= 1) {
            buf[cur] = buf[cur - 1];
        }
        uint32_t n = 0;
        for (; cur < len + 1; cur += 1) {
            tty_writeByte(buf[cur]);
            n += 1;
        }
        tty_cursorBack(n);
    }
    return 0;
}

// very simplistic, does not deal with line wraps.
const char* tty_readLine(char *buf, int buflen)
{
    uint32_t cursor = 0;
    uint32_t len = 0;
    while (1) {
        int32_t key = tty_readKey();
        switch (key) {
            case K_ESCAPE: { // nothing followed escape -> ESCAPE
                if (len > 0) tty_rlClearline(cursor, len);
                cursor = 0;
                len = 0;
                break;
            }
            case K_LEFT: if (cursor > 0) {
                cursor -= 1;
                tty_cursorBack(1);
                break;
            }
            case K_RIGHT: if (cursor < len) {
                cursor += 1;
                tty_cursorFwd(1);
                break;
            }
            case K_UP: {
                break;
            } // cursor up
            case K_DOWN: {
                break;
            } // cursor down
            case K_DEL: if (len > 0 && cursor < len) {
                if (tty_rlDelChar(buf, buflen, cursor, len)) len -= 1;
                break;
            }
            case K_CTRLA: if (cursor > 0) {
                tty_cursorBack(cursor);
                cursor = 0;
                break;
            }
            case K_CTRLD: if (len == 0) return NULL;
                break;
            case K_CTRLE: if (cursor < len) {
                tty_cursorFwd(len - cursor);
                cursor = len;
                break;
            }
            case K_CR: {
                tty_newline();
                buf[len] = 0;
                return buf;
                break;
            }
            case K_BACKSP: if (cursor > 0) {
                tty_cursorBack(1);
                if (tty_rlDelChar(buf, buflen, cursor - 1, len)) {
                    cursor -= 1;
                    len -= 1;
                }
                break;
            }
            default: if (key >= ' ' && key < 127 && len < buflen) {
                char byte = key & 0xff;
                if (tty_rlInsChar(buf, buflen, cursor, len) == 0) {
                    tty_writeByte(byte);
                    buf[cursor] = byte;
                    len += 1;
                    cursor += 1;
                }
                break;
            }
        }
    }
}

#endif /* RAWTTY_IMPLEMENTATION */

#endif /* #define RAWTTY_H */
