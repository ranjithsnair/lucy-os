// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.

#include <stdarg.h>
#include "types.h"
#include "defs.h"
#include "param.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "termios.h"

static void consputc(int);

static int panicked = 0;
// Which cpu set panicked, by LAPIC ID (not cpuid()/mycpu() - those
// require interrupts already disabled on the caller's own cpu, which
// consputc() below can't assume: a *different* cpu's ordinary cprintf()
// can land there with interrupts still enabled, before it's had any
// reason of its own to cli()) - see consputc()'s own comment.
static int panickedlapicid = -1;

static struct {
  struct spinlock lock;
  int locking;
} cons;

// xx is always the full pointer-width value here - %d/%x callers pass a
// plain int/uint (cprintf below reads it as such and it widens on the
// way in), %p callers pass a uintp directly - so this one implementation
// covers both without needing to know which.
static void
printint(uintp xx, int base, int sign)
{
  static char digits[] = "0123456789abcdef";
  char buf[24];
  int i;
  uintp x;

  if(sign && (long)xx < 0){
    x = -(long)xx;
  } else {
    x = xx;
    sign = 0;
  }

  i = 0;
  do{
    buf[i++] = digits[x % base];
  }while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}
//PAGEBREAK: 50

// Print to the console. only understands %d, %x, %p, %s.
//
// %d and %x read a plain int/uint (4 bytes, matching ordinary integer
// arguments); %p reads a full uintp (pointer-width) - the two must be
// kept distinct on the 64-bit build, where they're different sizes, so
// every %p call site needs to pass a pointer-width value (an address, a
// pde_t, etc.), not a plain int.
void
cprintf(char *fmt, ...)
{
  va_list ap;
  int i, c, locking;
  char *s;

  locking = cons.locking;
  if(locking)
    acquire(&cons.lock);

  if (fmt == 0)
    panic("null fmt");

  va_start(ap, fmt);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;
    case 'x':
      printint(va_arg(ap, uint), 16, 0);
      break;
    case 'p':
      printint(va_arg(ap, uintp), 16, 0);
      break;
    case 's':
      if((s = va_arg(ap, char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }
  va_end(ap);

  if(locking)
    release(&cons.lock);
}

void
panic(char *s)
{
  int i;
  uintp pcs[10];

  cli();
  cons.locking = 0;
  // Set before any cprintf() below, not after: consputc()'s own
  // panicked check is what makes another cpu already mid-print freeze
  // instead of interleaving with this one - found the hard way (a
  // multi-cpu VirtualBox boot where two cpus both panicked within the
  // same instant, each still holding cons.locking=0 and neither having
  // set panicked yet, garbling the console into interleaved bytes from
  // both cpus' cprintf() calls, letters and all). panickedlapicid
  // records which cpu got here first, so consputc() below freezes
  // every OTHER cpu but still lets this one finish its own message.
  panickedlapicid = lapicid();
  panicked = 1; // freeze other CPU
  // use lapiccpunum so that we can call panic from mycpu()
  cprintf("lapicid %d: panic: ", lapicid());
  cprintf(s);
  cprintf("\n");
  getcallerpcs(&s, pcs);
  for(i=0; i<10; i++)
    cprintf(" %p", pcs[i]);
  for(;;)
    ;
}

//PAGEBREAK: 50
#define BACKSPACE 0x100
#define CRTPORT 0x3d4
static ushort *crt = (ushort*)HW_P2V(0xb8000);  // CGA memory

// Current CGA text attribute byte (high byte of each crt[] cell) -
// mutated by the SGR handling in consolewrite() below (ESC[0m/ESC[7m),
// read by cgaputc() for every plain character it writes. 0x07 is the
// original hardcoded "black on white" this always used before curses
// needed reverse video for anything (status-bar highlighting).
static uchar consattr = 0x07;

static void
cgaputc(int c)
{
  int pos;

  // Cursor position: col + 80*row.
  outb(CRTPORT, 14);
  pos = inb(CRTPORT+1) << 8;
  outb(CRTPORT, 15);
  pos |= inb(CRTPORT+1);

  if(c == '\n')
    pos += 80 - pos%80;
  else if(c == BACKSPACE){
    if(pos > 0) --pos;
  } else
    crt[pos++] = (c&0xff) | (consattr<<8);

  if(pos < 0 || pos > 25*80)
    panic("pos under/overflow");

  // Scroll up once the cursor would move past the last of the CGA's 25
  // physical rows (rows 0-24) - previously this triggered a row early,
  // at row 24 (leaving the last physical row permanently unused); a
  // curses full-screen layout needs every row, e.g. a status bar drawn
  // via direct cursor-addressing (see consetcursor() below) right at
  // the bottom of the screen, so this now uses the whole 25 rows.
  if((pos/80) >= 25){  // Scroll up.
    memmove(crt, crt+80, sizeof(crt[0])*24*80);
    pos -= 80;
    memset(crt+pos, 0, sizeof(crt[0])*(25*80 - pos));
  }

  outb(CRTPORT, 14);
  outb(CRTPORT+1, pos>>8);
  outb(CRTPORT, 15);
  outb(CRTPORT+1, pos);
  crt[pos] = ' ' | (consattr<<8);
}

// Absolute cell index (row*80+col) the CGA hardware cursor currently
// sits at - the same CRTPORT read cgaputc() does, factored out so the
// ESC[K (clear-to-EOL) handler below can find "the rest of this row"
// without moving the cursor first.
static int
conscursorpos(void)
{
  int pos;

  outb(CRTPORT, 14);
  pos = inb(CRTPORT+1) << 8;
  outb(CRTPORT, 15);
  pos |= inb(CRTPORT+1);
  return pos;
}

// CUP (ESC[{row};{col}H): move the hardware cursor directly, the same
// register writes cgaputc() ends with. row/col are 1-indexed (ANSI
// convention) and clamped rather than trusted, so a stray/malformed
// escape sequence can't drive pos outside the 25x80 grid and hit
// cgaputc()'s "pos under/overflow" panic - unlike cgaputc()'s own pos,
// which only ever comes from the kernel's own arithmetic, this one
// originates in a userspace-supplied byte stream.
static void
consetcursor(int row, int col)
{
  int pos;

  if(row < 1) row = 1;
  if(row > 25) row = 25;
  if(col < 1) col = 1;
  if(col > 80) col = 80;
  pos = (row-1)*80 + (col-1);

  outb(CRTPORT, 14);
  outb(CRTPORT+1, pos>>8);
  outb(CRTPORT, 15);
  outb(CRTPORT+1, pos);
}

// ED (ESC[2J): clear the whole grid to blank cells in the current
// attribute, cursor position unchanged - matching real ANSI ED, unlike
// cgaputc()'s scroll-clear above (which zeroes bytes, not attributed
// spaces, since nothing ever displays that transiently-scrolled-off
// region).
static void
consclearscreen(void)
{
  int i;

  for(i = 0; i < 25*80; i++)
    crt[i] = ' ' | (consattr<<8);
}

// EL (ESC[K, or ESC[0K): clear from the cursor to the end of its
// current row.
static void
consclearline(void)
{
  int pos = conscursorpos();
  int end = (pos/80 + 1) * 80;
  int i;

  for(i = pos; i < end; i++)
    crt[i] = ' ' | (consattr<<8);
}

// Escape-sequence parser state for consolewrite() below - persists
// across calls (rather than resetting per-call) since a single
// caller's write() isn't guaranteed to contain a whole escape
// sequence. Recognizes plain ANSI/VT100 CSI sequences only (ESC '['
// digits/';' final-letter) - enough for the minimal curses
// implementation (curses/curses.c) to do cursor-addressed screen
// redraws; nothing else in this OS's console ever emits ESC.
#define ESC_MAXPARAMS 2
static struct {
  enum { ESC_NONE, ESC_ESC, ESC_CSI } state;
  int params[ESC_MAXPARAMS];
  int nparams;
} escstate;

// Apply one recognized CSI sequence's effect to the CGA framebuffer -
// called from consolewrite() once the final byte of a sequence is
// seen. Only CUP/ED/EL/SGR are interpreted (see this file's own
// comment on escstate); any other final byte, or any ED/EL parameter
// other than the "whole screen"/"to end of line" ones curses actually
// emits, is silently accepted and ignored rather than rejected -
// consolewrite() has already forwarded the raw bytes to uartputc()
// regardless (a real terminal on the other end of the serial line
// interprets the full sequence itself; this only needs to cover what
// this OS's own curses.c generates for the CGA path).
static void
conshandlecsi(int final, int *params, int nparams)
{
  int row, col, i;

  switch(final){
  case 'H':  // CUP
    row = (nparams >= 1 && params[0] > 0) ? params[0] : 1;
    col = (nparams >= 2 && params[1] > 0) ? params[1] : 1;
    consetcursor(row, col);
    break;
  case 'J':  // ED
    if(nparams >= 1 && params[0] == 2)
      consclearscreen();
    break;
  case 'K':  // EL
    if(nparams == 0 || params[0] == 0)
      consclearline();
    break;
  case 'm':  // SGR - apply every parameter in order, like a real
             // terminal would (e.g. "ESC[0;7m" resets then reverses)
    if(nparams == 0){
      consattr = 0x07;
    } else {
      for(i = 0; i < nparams; i++){
        if(params[i] == 0)
          consattr = 0x07;
        else if(params[i] == 7)
          consattr = 0x70;
        // any other SGR code (bold, etc.) is accepted, ignored
      }
    }
    break;
  }
}

void
consputc(int c)
{
  if(panicked && lapicid() != panickedlapicid){
    cli();
    for(;;)
      ;
  }

  if(c == BACKSPACE){
    uartputc('\b'); uartputc(' '); uartputc('\b');
  } else
    uartputc(c);
  cgaputc(c);
}

// The input ring buffer has three indices, not the usual two, because
// input is line-buffered: characters accumulate between w and e as the
// user types and backspaces, and only become available to consoleread()
// (by advancing w to e) once a full line - or ^D, or a full buffer - is
// seen. r trails behind, marking how much of the confirmed (w-bounded)
// input consoleread() has already consumed.
#define INPUT_BUF 128
struct {
  char buf[INPUT_BUF];
  uint r;  // Read index
  uint w;  // Write index (of already-terminated, readable input)
  uint e;  // Edit index (of the line still being typed)
} input;

#define C(x)  ((x)-'@')  // Control-x

// Console termios state - see include/termios.h's own comment. contermios
// is whatever a program last handed tcsetattr(); rawmode/rawecho are
// derived from its c_lflag so consoleintr()/consoleread() (both hot,
// per-character paths) can check two plain ints instead of re-testing
// flag bits every keystroke. Defaults are ordinary cooked-mode terminal
// behavior, matching what every fd has always gotten on this console
// before ioctl(TCSETS) existed at all.
static struct termios contermios = {
  .c_iflag = 0,
  .c_oflag = 0,
  .c_cflag = 0,
  .c_lflag = ISIG | ICANON | ECHO,
  .c_line = 0,
  .c_cc = { [VEOF] = C('D'), [VTIME] = 0, [VMIN] = 1 },
  .__c_ispeed = 0,
  .__c_ospeed = 0,
};
static int rawmode = 0;
static int rawecho = 1;

// Called with cons.lock held (or not yet initialized) - both callers
// (consolesettermios() and the static initializer above) already
// satisfy this.
static void
deriverawstate(void)
{
  rawmode = (contermios.c_lflag & ICANON) == 0;
  rawecho = (contermios.c_lflag & ECHO) != 0;
}

// TCGETS: report the termios a prior TCSETS installed (or the cooked-
// mode default above, if none ever was).
void
consolegettermios(struct termios *tio)
{
  acquire(&cons.lock);
  *tio = contermios;
  release(&cons.lock);
}

// TCSETS/TCSETSW/TCSETSF: this console has no output queue to drain and
// no pending input to flush, so all three act identically - just
// install the new termios and re-derive rawmode/rawecho from it.
void
consolesettermios(struct termios *tio)
{
  acquire(&cons.lock);
  contermios = *tio;
  deriverawstate();
  release(&cons.lock);
}

// TIOCGWINSZ: this console is always the CGA text console's fixed
// 80x25 (see CRTPORT/25*80 above) - no live-resize (SYS_ioctl's own
// comment: no signal delivery exists to report one via SIGWINCH anyway).
void
consolegetwinsize(struct winsize *ws)
{
  ws->ws_row = 25;
  ws->ws_col = 80;
  ws->ws_xpixel = 0;
  ws->ws_ypixel = 0;
}

void
consoleintr(int (*getc)(void))
{
  int c, doprocdump = 0;

  acquire(&cons.lock);
  while((c = getc()) >= 0){
    if(rawmode && c != C('P')){
      // Raw mode: the app owns line editing, so ^U/^H/^D get no
      // special kernel treatment - every byte (including those) goes
      // straight into the buffer, and the reader is woken after each
      // one rather than waiting for a newline (VMIN=1/VTIME=0-style
      // delivery - see consoleread()'s matching early-return).
      if(c != 0 && input.e-input.r < INPUT_BUF){
        input.buf[input.e++ % INPUT_BUF] = c;
        if(rawecho)
          consputc(c);
        input.w = input.e;
        wakeup(&input.r);
      }
      continue;
    }
    switch(c){
    case C('P'):  // Process listing.
      // procdump() locks cons.lock indirectly; invoke later
      doprocdump = 1;
      break;
    case C('U'):  // Kill line.
      while(input.e != input.w &&
            input.buf[(input.e-1) % INPUT_BUF] != '\n'){
        input.e--;
        consputc(BACKSPACE);
      }
      break;
    case C('H'): case '\x7f':  // Backspace
      if(input.e != input.w){
        input.e--;
        consputc(BACKSPACE);
      }
      break;
    default:
      if(c != 0 && input.e-input.r < INPUT_BUF){
        c = (c == '\r') ? '\n' : c;
        input.buf[input.e++ % INPUT_BUF] = c;
        consputc(c);
        if(c == '\n' || c == C('D') || input.e == input.r+INPUT_BUF){
          input.w = input.e;
          wakeup(&input.r);
        }
      }
      break;
    }
  }
  release(&cons.lock);
  if(doprocdump) {
    procdump();  // now call procdump() wo. cons.lock held
  }
}

// Readiness check for kernel/epoll.c's fdready() - GUI roadmap phase
// 8's compositor is the first program to epoll a device fd (this one
// and kernel/mouse.c's) alongside a real socket, exposing fdready()'s
// old "default: return 1" for anything that isn't FD_PIPE/FD_SOCK:
// that default is correct for an ordinary disk file (a read() there
// never blocks waiting for more data to arrive) but wrong for the
// console, whose consoleread() genuinely blocks until a real keystroke
// lands - epoll_wait() would otherwise always report it ready, and a
// caller's subsequent read() would then hang the whole event loop the
// first time nothing was actually typed yet. input.r != input.w is
// exactly what consoleread()'s own wait loop below already checks.
int
consolereadable(void)
{
  int r;

  acquire(&cons.lock);
  r = (input.r != input.w);
  release(&cons.lock);
  return r;
}

int
consoleread(struct inode *ip, char *dst, int n)
{
  uint target;
  int c;

  iunlock(ip);
  target = n;
  acquire(&cons.lock);
  while(n > 0){
    while(input.r == input.w){
      if(myproc()->killed){
        release(&cons.lock);
        ilock(ip);
        return -1;
      }
      // VMIN=0 (curses' nodelay(), via TCSETS): a real non-blocking
      // read, not a "wait for the first byte" one - return whatever
      // has already been copied this call (possibly 0) immediately,
      // rather than sleeping for a byte that may not be coming. Every
      // other combination this console supports (cooked mode, or raw
      // mode's default VMIN=1) still blocks here as before.
      if(rawmode && contermios.c_cc[VMIN] == 0){
        release(&cons.lock);
        ilock(ip);
        return target - n;
      }
      sleep(&input.r, &cons.lock);
    }
    c = input.buf[input.r++ % INPUT_BUF];
    if(!rawmode && c == C('D')){  // EOF (cooked mode only - see below)
      if(n < target){
        // Save ^D for next time, to make sure
        // caller gets a 0-byte result.
        input.r--;
      }
      break;
    }
    *dst++ = c;
    --n;
    if(!rawmode && c == '\n')
      break;
    if(rawmode && input.r == input.w)
      // VMIN=1/VTIME=0-style delivery: return as soon as nothing more
      // is buffered, rather than waiting to fill dst or see a '\n' -
      // consoleintr() already woke us after every single character.
      // ^D is ordinary data here (byte 0x04), not EOF - real termios
      // only treats it specially in canonical mode.
      break;
  }
  release(&cons.lock);
  ilock(ip);

  return target - n;
}

int
consolewrite(struct inode *ip, char *buf, int n)
{
  int i, c;

  iunlock(ip);
  acquire(&cons.lock);
  for(i = 0; i < n; i++){
    c = buf[i] & 0xff;
    switch(escstate.state){
    case ESC_NONE:
      if(c == 0x1b){
        escstate.state = ESC_ESC;
        uartputc(c);  // forward verbatim - a real terminal on the
        continue;     // other end of the serial line reads this too
      }
      break;
    case ESC_ESC:
      uartputc(c);
      if(c == '['){
        escstate.state = ESC_CSI;
        escstate.nparams = 0;
        escstate.params[0] = 0;
      } else {
        // Not a CSI sequence after all - only ESC '[' is supported,
        // so just drop back to normal state; the ESC itself was
        // already forwarded to uartputc() above, and c here isn't a
        // display character to also hand to cgaputc().
        escstate.state = ESC_NONE;
      }
      continue;
    case ESC_CSI:
      uartputc(c);
      if(c >= '0' && c <= '9'){
        if(escstate.nparams == 0)
          escstate.nparams = 1;
        escstate.params[escstate.nparams-1] =
          escstate.params[escstate.nparams-1]*10 + (c-'0');
        continue;
      }
      if(c == ';'){
        if(escstate.nparams < ESC_MAXPARAMS){
          escstate.nparams++;
          escstate.params[escstate.nparams-1] = 0;
        }
        continue;
      }
      // Final byte: apply it to the CGA framebuffer (uart already has
      // every raw byte of the sequence, forwarded above as it arrived).
      conshandlecsi(c, escstate.params, escstate.nparams);
      escstate.state = ESC_NONE;
      continue;
    }
    consputc(c);
  }
  release(&cons.lock);
  ilock(ip);

  return n;
}

void
consoleinit(void)
{
  initlock(&cons.lock, "console");

  devsw[CONSOLE].write = consolewrite;
  devsw[CONSOLE].read = consoleread;
  cons.locking = 1;

  ioapicenable(IRQ_KBD, 0);
}

