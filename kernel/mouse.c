// PS/2 mouse driver (GUI roadmap phase 3) - mirrors kernel/kbd.c's own
// keyboard-IRQ pattern closely: same 8042 controller, second (aux)
// port instead of the first, IRQ12 instead of IRQ1. Decodes the
// standard 3-byte PS/2 packet (button state + signed 9-bit dx/dy) into
// a small ring buffer of whole packets - read() always returns whole
// 3-byte records, never a partial one, the same "always return whole
// records" simplification kernel/socket.c's sockmsg already uses and
// for the same reason: dead simple, and sufficient for a test program
// or eventual compositor consumer.

#include "types.h"
#include "x86.h"
#include "defs.h"
#include "param.h"
#include "traps.h"
#include "mmu.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "proc.h"

// Same 8042 controller ports as include/kbd.h's KBSTATP/KBDATAP -
// not included directly here since kbd.h also declares kbd.c's own
// scancode-to-ASCII tables, which this file has no use for.
#define MOUSE_KBSTATP 0x64
#define MOUSE_KBDATAP 0x60

#define MOUSEBUFSIZE 256  // queued packets

static struct {
  struct spinlock lock;
  uchar buf[MOUSEBUFSIZE][3];
  uint nread, nwrite;   // monotonic packet counts, mod MOUSEBUFSIZE
                          // for indexing - same "never wrap the
                          // counters themselves" trick kernel/pipe.c's
                          // struct pipe already uses
} mouse;

static void
mousepush(uchar *p)
{
  acquire(&mouse.lock);
  if(mouse.nwrite - mouse.nread < MOUSEBUFSIZE){
    uchar *slot = mouse.buf[mouse.nwrite % MOUSEBUFSIZE];
    slot[0] = p[0];
    slot[1] = p[1];
    slot[2] = p[2];
    mouse.nwrite++;
  }
  // else: buffer full - drop the packet rather than block an
  // interrupt handler or overwrite unread data out from under a
  // reader; a mouse's own next packet supersedes it anyway.
  wakeup(&mouse);
  release(&mouse.lock);
}

void
mouseintr(void)
{
  static uchar cycle = 0;
  static uchar packet[3];
  uchar data;

  data = inb(MOUSE_KBDATAP);  // 0x60 - shared with the keyboard controller,
                          // safe here since this only runs on IRQ12
  switch(cycle){
  case 0:
    // Byte 0 of a real PS/2 mouse packet always has bit 3 set (a sync
    // bit). If it's not set, the byte stream desynced (e.g. this
    // driver attached mid-packet) - drop it and keep waiting for a
    // valid start rather than accumulating garbage into packet[].
    if(!(data & 0x08))
      return;
    packet[0] = data;
    cycle = 1;
    break;
  case 1:
    packet[1] = data;
    cycle = 2;
    break;
  case 2:
    packet[2] = data;
    cycle = 0;
    mousepush(packet);
    break;
  }
}

// Readiness check for kernel/epoll.c's fdready() - see
// kernel/console.c's consolereadable() for the full reasoning (same
// underlying gap: fdready()'s old default assumed every non-pipe/
// non-socket fd behaves like an ordinary disk file, never blocking a
// read() - false for this device too, whose mouseread() genuinely
// blocks until a real PS/2 packet arrives).
int
mousereadable(void)
{
  int r;

  acquire(&mouse.lock);
  r = (mouse.nread != mouse.nwrite);
  release(&mouse.lock);
  return r;
}

int
mouseread(struct inode *ip, char *dst, int n)
{
  int i, npkts, avail;
  uchar *p;

  acquire(&mouse.lock);
  while(mouse.nwrite == mouse.nread){
    if(myproc()->killed){
      release(&mouse.lock);
      return -1;
    }
    sleep(&mouse, &mouse.lock);
  }
  avail = mouse.nwrite - mouse.nread;
  npkts = n / 3;
  if(npkts > avail)
    npkts = avail;
  for(i = 0; i < npkts; i++){
    p = mouse.buf[mouse.nread % MOUSEBUFSIZE];
    dst[3*i] = p[0];
    dst[3*i+1] = p[1];
    dst[3*i+2] = p[2];
    mouse.nread++;
  }
  release(&mouse.lock);
  return npkts * 3;
}

int
mousewrite(struct inode *ip, char *buf, int n)
{
  return -1;  // nothing to write to a mouse
}

// Waits for the 8042 controller to be ready for the next step of the
// init sequence below - type 0: input buffer clear (safe to write a
// command/data byte); type 1: output buffer full (a byte is waiting to
// be read). Bounded, not indefinite: a QEMU/VirtualBox/real machine
// with no PS/2 controller at all (rare, but possible on real hardware)
// must not hang boot - it should just end up with a mouse that never
// reports anything, the same "missing hardware degrades gracefully"
// approach kernel/vbe.c already takes for a VBE-less BIOS.
static int
mousewait(int type)
{
  int timeout = 100000;

  if(type == 0){
    while(timeout-- > 0 && (inb(MOUSE_KBSTATP) & 0x02))
      ;
  } else {
    while(timeout-- > 0 && !(inb(MOUSE_KBSTATP) & 0x01))
      ;
  }
  return timeout > 0;
}

void
mouseinit(void)
{
  uchar status;
  int ok = 1;

  ok &= mousewait(0);
  outb(MOUSE_KBSTATP, 0xA8);       // enable the auxiliary (mouse) device

  ok &= mousewait(0);
  outb(MOUSE_KBSTATP, 0x20);       // "read controller command byte"
  ok &= mousewait(1);
  status = inb(MOUSE_KBDATAP);
  status |= 0x02;              // bit1: enable IRQ12
  status &= ~(uchar)0x20;      // bit5: enable aux clock (0 = enabled)
  ok &= mousewait(0);
  outb(MOUSE_KBSTATP, 0x60);       // "write controller command byte"
  ok &= mousewait(0);
  outb(MOUSE_KBDATAP, status);

  ok &= mousewait(0);
  outb(MOUSE_KBSTATP, 0xD4);       // next data byte goes to the aux device
  ok &= mousewait(0);
  outb(MOUSE_KBDATAP, 0xF4);       // "enable data reporting"
  ok &= mousewait(1);
  inb(MOUSE_KBDATAP);              // discard the 0xFA ack

  // Every step above is bounded (mousewait()'s own comment), so a
  // stuck/missing 8042 aux port degrades to "mouse never reports
  // anything" rather than hanging boot - but silently: nothing else
  // here ever surfaced that. Report it, at least, in case a future
  // "mouse doesn't work" report on real hardware traces back to this.
  if(!ok)
    cprintf("mouseinit: 8042 handshake timed out - mouse may not work\n");

  initlock(&mouse.lock, "mouse");
  devsw[MOUSE].read = mouseread;
  devsw[MOUSE].write = mousewrite;

  ioapicenable(IRQ_MOUSE, 0);
}
