//
// File descriptors
//
// The kernel-wide open file table: struct file here is what a process's
// per-process fd table (proc->ofile[]) points at. Several file
// descriptors, in the same or different processes, can share one struct
// file (via dup(), fork(), and open() on the same file more than once),
// which is what f->ref tracks - the underlying inode or pipe is only
// actually closed once the last such reference goes away.
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"

// Dispatch table for device files (T_DEV inodes), indexed by major
// number - see readi/writei in fs.c, which call through it for T_DEV
// inodes instead of reading/writing disk blocks. The console driver
// registers itself here (see console.c).
struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE)
    pipeclose(ff.pipe, ff.writable);
  else if(ff.type == FD_INODE){
    begin_op();
    iput(ff.ip);
    end_op();
  } else if(ff.type == FD_SOCK)
    sockclose(ff.sock);
  else if(ff.type == FD_SHM)
    shmclose(ff.shm);
  else if(ff.type == FD_EPOLL)
    kfree((char*)ff.epoll);
}

// Get metadata about file f.
int
filestat(struct file *f, struct stat *st)
{
  if(f->type == FD_INODE){
    ilock(f->ip);
    stati(f->ip, st);
    iunlock(f->ip);
    return 0;
  }
  return -1;
}

// Read from file f.
int
fileread(struct file *f, char *addr, int n)
{
  int r;

  if(f->readable == 0)
    return -1;
  if(f->type == FD_PIPE)
    return piperead(f->pipe, addr, n);
  // FD_SOCK: plain read() falls back to sockrecv() with no fd slots -
  // any SCM_RIGHTS ancillary data riding along a message consumed this
  // way is dropped, matching real recvmsg()-vs-read() semantics on a
  // real Unix socket (see include/socket.h's own scope-cut comment on
  // why sendmsg()/recvmsg() are the primary path here).
  if(f->type == FD_SOCK){
    int nfds;
    return sockrecv(f->sock, addr, n, 0, &nfds, 0);
  }
  if(f->type == FD_SHM || f->type == FD_EPOLL)
    return -1;  // mmap()/epoll_ctl() only - not a byte stream
  if(f->type == FD_INODE){
    // Fast path: readi_dense() (kernel/fs.c) walks the block map with
    // non-allocating, batched peeks instead of bmap()'s allocating
    // one-bread()-per-block walk, and succeeds unless it actually hits
    // a hole - true for every ordinary file (mkfs's own image never
    // creates one; only itruncto() growing ip->size without writing
    // does, see its comment). On success it can skip the transaction
    // machinery below entirely, since it's already proven nothing
    // needed allocating. This is the common case, and matters: without
    // it, *every* read - regardless of whether it could ever touch a
    // hole - paid the small-chunked transaction loop's per-chunk
    // log-commit cost below, measured turning a single ~700KB TTF font
    // read into several real seconds of nothing but log commits (and,
    // in an earlier version of this fix that checked density with a
    // separate per-block pass first, several more seconds of nothing
    // but redundant lock acquisitions - see readi_dense()'s own
    // comment) for data that was never actually going to touch the
    // log.
    ilock(f->ip);
    r = readi_dense(f->ip, addr, f->off, n);
    if(r >= 0){
      f->off += r;
      iunlock(f->ip);
      return r;
    }
    iunlock(f->ip);

    // Slow path: this read's range includes (or might include) a hole
    // left by itruncto() growing ip->size without writing, so bmap()
    // may need its allocating branch after all - back to the original
    // transaction-per-chunk loop, unchanged, for correctness. A
    // transaction (matching filewrite()'s own, just below) is needed
    // here even though a read "obviously" never used to touch the
    // log: itruncto() (kernel/fs.c, ftruncate(2)'s backing logic) can
    // grow ip->size past every block that's actually been allocated,
    // on the theory that bmap() lazily allocates - and zeroes - a
    // block the first time anything touches it, read or write, so a
    // read of that grown region reads back zeros without this port
    // needing to track holes as their own concept. That lazy
    // allocation calls balloc(), which calls log_write() - the one
    // part of readi() that can now actually modify the disk, where
    // before (nothing could ever grow ip->size without already having
    // allocated every block up to the new size) it never did. Chunked
    // like filewrite()'s own loop, for the same reason: a single read
    // of a large enough hole could otherwise need to allocate more
    // blocks than one transaction is sized for.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * 512;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      r = readi(f->ip, addr + i, f->off, n1);
      if(r > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r < 0)
        return i > 0 ? i : -1;
      i += r;
      if(r < n1)
        break;  // short read: hit EOF, no point asking for more
    }
    return i;
  }
  panic("fileread");
}

//PAGEBREAK!
// Write to file f.
int
filewrite(struct file *f, char *addr, int n)
{
  int r;

  if(f->writable == 0)
    return -1;
  if(f->type == FD_PIPE)
    return pipewrite(f->pipe, addr, n);
  if(f->type == FD_SOCK)
    return socksend(f->sock, addr, n, 0, 0);
  if(f->type == FD_SHM || f->type == FD_EPOLL)
    return -1;  // mmap()/epoll_ctl() only - not a byte stream
  if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * 512;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r < 0)
        break;
      if(r != n1)
        panic("short filewrite");
      i += r;
    }
    return i == n ? n : -1;
  }
  panic("filewrite");
}

