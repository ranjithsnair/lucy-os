// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/poc/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
static void itrunc(struct inode*);
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

// Read the super block.
void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free?
        bp->data[bi/8] |= m;  // Mark block in use.
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

void
iinit(int dev)
{
  int i = 0;
  
  initlock(&icache.lock, "icache");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d ninodes %d nlog %d logstart %d\
 inodestart %d bmap start %d\n", sb.size, sb.nblocks,
          sb.ninodes, sb.nlog, sb.logstart, sb.inodestart,
          sb.bmapstart);
}

static struct inode* iget(uint dev, uint inum);

//PAGEBREAK!
// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->mode = ip->mode;
  dip->uid = ip->uid;
  dip->gid = ip->gid;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->mode = dip->mode;
    ip->uid = dip->uid;
    ip->gid = dip->gid;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquiresleep(&ip->lock);
  if(ip->valid && ip->nlink == 0){
    acquire(&icache.lock);
    int r = ip->ref;
    release(&icache.lock);
    if(r == 1){
      // inode has no links and no other references: truncate and free.
      itrunc(ip);
      ip->type = 0;
      iupdate(ip);
      ip->valid = 0;
    }
  }
  releasesleep(&ip->lock);

  acquire(&icache.lock);
  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

//PAGEBREAK!
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
//
// The first NDIRECT blocks of a file are addressed directly out of
// ip->addrs[0..NDIRECT-1]. Beyond that, ip->addrs[NDIRECT] points at one
// more block - the "indirect block" - which itself holds up to NINDIRECT
// more block addresses, extending the maximum file size at the cost of
// one extra disk read per access to those blocks. Beyond THAT,
// ip->addrs[NDIRECT+1] points at a "doubly-indirect block": NINDIRECT
// pointers to further indirect blocks, each in turn holding NINDIRECT
// data-block addresses - two extra disk reads per access, but room for
// NINDIRECT*NINDIRECT more blocks (see include/fs.h's MAXFILE/NDINDIRECT
// comment for why this was added).
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((addr = a[bn]) == 0){
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }
  bn -= NINDIRECT;

  if(bn < NDINDIRECT){
    uint idx1 = bn / NINDIRECT;   // which indirect block, within the doubly-indirect block
    uint idx2 = bn % NINDIRECT;   // which data block, within that indirect block
    uint iaddr;
    struct buf *bp2;
    uint *a2;

    // Load the doubly-indirect block, allocating if necessary.
    if((addr = ip->addrs[NDIRECT+1]) == 0)
      ip->addrs[NDIRECT+1] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    if((iaddr = a[idx1]) == 0){
      a[idx1] = iaddr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);

    // Load the singly-indirect block it points to, allocating if
    // necessary.
    bp2 = bread(ip->dev, iaddr);
    a2 = (uint*)bp2->data;
    if((addr = a2[idx2]) == 0){
      a2[idx2] = addr = balloc(ip->dev);
      log_write(bp2);
    }
    brelse(bp2);
    return addr;
  }

  panic("bmap: out of range");
}

// Read-only counterpart to bmap(): returns the already-allocated block
// address for bn, or 0 if that block (or an indirect/doubly-indirect
// pointer block on the way to it) hasn't been allocated yet - i.e. bn
// falls in a hole left by itruncto() growing ip->size without writing
// (see that function's own comment). Never calls balloc()/log_write(),
// so - unlike bmap() - this is safe to call outside a begin_op()/
// end_op() transaction: only bread()s, which don't touch the log.
static uint
bmap_mapped(struct inode *ip, uint bn)
{
  uint addr, iaddr, *a;
  struct buf *bp;

  if(bn < NDIRECT)
    return ip->addrs[bn];
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    if((addr = ip->addrs[NDIRECT]) == 0)
      return 0;
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    addr = a[bn];
    brelse(bp);
    return addr;
  }
  bn -= NINDIRECT;

  if(bn < NDINDIRECT){
    uint idx1 = bn / NINDIRECT;
    uint idx2 = bn % NINDIRECT;

    if((addr = ip->addrs[NDIRECT+1]) == 0)
      return 0;
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    iaddr = a[idx1];
    brelse(bp);
    if(iaddr == 0)
      return 0;

    bp = bread(ip->dev, iaddr);
    a = (uint*)bp->data;
    addr = a[idx2];
    brelse(bp);
    return addr;
  }

  panic("bmap_mapped: out of range");
}

// Batched counterpart to calling bmap_mapped() once per block: extends
// from logical block bn as far as maxblocks blocks that are both
// already-allocated and physically contiguous with the first, but
// reuses each indirect/doubly-indirect pointer block's buffer across
// every logical block that shares it - one bread()/brelse() pair (and
// so one spinlock acquire()/release() pair) per *pointer* block
// actually touched, not one per data block. That distinction is what
// actually matters: every acquire() runs mycpu() (kernel/proc.c),
// which reads the Local APIC ID via an MMIO access - cheap on real
// hardware/QEMU, but measured to be expensive enough under VirtualBox
// specifically that calling bmap_mapped() once per block turned a
// single ~700KB file read (see readi_dense()'s own comment for the
// concrete numbers) into several real seconds of nothing but lock
// acquisitions for data that was always going to be a cache hit.
// Returns the run length (0 if bn itself is a hole) and, if nonzero,
// the physical address of the first block via *first_addr. Never
// calls balloc()/log_write() - safe outside any transaction.
static uint
contig_dense_run(struct inode *ip, uint bn, uint maxblocks, uint *first_addr)
{
  uint count = 0, addr;
  struct buf *bp, *bp2;
  uint *a, *a2;

  if(maxblocks == 0)
    return 0;

  // Direct blocks: already resident in ip->addrs[], no bread needed.
  while(count < maxblocks && bn + count < NDIRECT){
    addr = ip->addrs[bn + count];
    if(addr == 0)
      return count;
    if(count == 0)
      *first_addr = addr;
    else if(addr != *first_addr + count)
      return count;
    count++;
  }
  if(count == maxblocks || bn + count >= NDIRECT + NINDIRECT + NDINDIRECT)
    return count;

  // Single-indirect range: one bread() covers every block from
  // NDIRECT up to NDIRECT+NINDIRECT-1.
  if(bn + count < NDIRECT + NINDIRECT){
    if(ip->addrs[NDIRECT] == 0)
      return count;
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    while(count < maxblocks && bn + count < NDIRECT + NINDIRECT){
      addr = a[bn + count - NDIRECT];
      if(addr == 0){
        brelse(bp);
        return count;
      }
      if(count == 0)
        *first_addr = addr;
      else if(addr != *first_addr + count){
        brelse(bp);
        return count;
      }
      count++;
    }
    brelse(bp);
    if(count == maxblocks)
      return count;
  }

  // Doubly-indirect range: one bread() for the doubly-indirect block
  // itself, then one bread() per singly-indirect leaf block actually
  // touched (each covering up to NINDIRECT data blocks) - not one per
  // data block.
  if(ip->addrs[NDIRECT+1] == 0)
    return count;
  bp = bread(ip->dev, ip->addrs[NDIRECT+1]);
  a = (uint*)bp->data;
  while(count < maxblocks){
    uint off2 = bn + count - NDIRECT - NINDIRECT;
    uint idx1 = off2 / NINDIRECT;
    uint iaddr;

    if(idx1 >= NINDIRECT)
      break;
    iaddr = a[idx1];
    if(iaddr == 0)
      break;
    bp2 = bread(ip->dev, iaddr);
    a2 = (uint*)bp2->data;
    while(count < maxblocks){
      uint o2 = bn + count - NDIRECT - NINDIRECT;

      if(o2 / NINDIRECT != idx1)
        break;
      addr = a2[o2 % NINDIRECT];
      if(addr == 0){
        brelse(bp2);
        brelse(bp);
        return count;
      }
      if(count == 0)
        *first_addr = addr;
      else if(addr != *first_addr + count){
        brelse(bp2);
        brelse(bp);
        return count;
      }
      count++;
    }
    brelse(bp2);
  }
  brelse(bp);
  return count;
}

// Fast counterpart to readi(), for fileread()'s dense-file case only:
// structured exactly like readi()'s own bulk-vs-single-block split,
// but walks the block map with contig_dense_run()/bmap_mapped()
// (batched, non-allocating) instead of bmap() (which allocates on a
// hole and therefore needs a transaction around it). Never touches
// the log, so it's safe to call outside any begin_op()/end_op() pair.
// Returns -1 the moment it would hit a hole - whatever's already been
// written into dst is fine to leave as-is when that happens, since the
// caller (fileread()) discards this attempt entirely and retries the
// whole read via the original transactional path rather than trying
// to resume a partial dense read.
int
readi_dense(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m, addr;
  struct buf *bp;

  // A device special file (T_DEV - console, mouse, framebuffer) has no
  // real block-mapped data at all, and ialloc() leaves every fresh
  // inode's on-disk size at 0 (memset to zero, never touched again for
  // a device - see ialloc()'s own memset()). Falling through to the
  // off/size clamping below would therefore clamp n to 0 for *every*
  // device read (0 + n > size(0) is always true), silently "succeeding"
  // with a 0-byte, EOF-looking read instead of ever reaching the real
  // driver - which for the mouse device in particular means real,
  // already-decoded PS/2 packets (kernel/mouse.c) sit in its ring
  // buffer forever, unconsumed, and the cursor never moves. Matches
  // readi()'s own T_DEV branch (which this whole function otherwise
  // parallels) - checked first, before any of that arithmetic.
  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    if(off % BSIZE == 0 && n - tot >= BSIZE){
      uint first_addr = 0, nb = contig_dense_run(ip, off / BSIZE, (n - tot) / BSIZE, &first_addr);

      if(nb > 0 && bio_range_clean(ip->dev, first_addr, nb)){
        ide_bulk_read(first_addr, dst, nb);
        m = nb * BSIZE;
        continue;
      }
      if(nb == 0)
        return -1;
      // nb > 0 but dirty: fall through to the single-block path below
      // for just this one block, same as readi()'s own such fallback.
    }
    if((addr = bmap_mapped(ip, off / BSIZE)) == 0)
      return -1;
    bp = bread(ip->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(dst, bp->data + off%BSIZE, m);
    brelse(bp);
  }
  return tot;
}

// Free every data block of ip whose logical block number is >= startbn
// (itrunc(ip)'s startbn==0 case discards everything; sys_ftruncate's
// shrink case passes ceil(newsize/BSIZE) to keep the blocks before the
// new size intact). An indirect/doubly-indirect block's own pointer
// block is freed too, but only once every entry it can reach is at or
// past startbn - i.e. iff startbn is at or before that level's first
// logical block number - since otherwise an earlier, still-live entry
// is hanging off the same pointer block. Doesn't touch ip->size or
// call iupdate(); callers do that themselves (itrunc() always sets
// size to 0, sys_ftruncate() to the caller-requested length, which
// unlike itrunc() may be larger than the old size).
static void
itruncfrom(struct inode *ip, uint startbn)
{
  int i, j;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    if((uint)i >= startbn && ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if(ip->addrs[NDIRECT] && startbn < NDIRECT + NINDIRECT){
    int changed = 0;
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint*)bp->data;
    for(j = 0; j < NINDIRECT; j++){
      if((uint)(NDIRECT + j) >= startbn && a[j]){
        bfree(ip->dev, a[j]);
        a[j] = 0;
        changed = 1;
      }
    }
    if(changed)
      log_write(bp);
    brelse(bp);
    if(startbn <= NDIRECT){
      bfree(ip->dev, ip->addrs[NDIRECT]);
      ip->addrs[NDIRECT] = 0;
    }
  }

  if(ip->addrs[NDIRECT+1] && startbn < NDIRECT + NINDIRECT + NDINDIRECT){
    struct buf *bp2;
    uint *a2;
    int outer_changed = 0;

    bp = bread(ip->dev, ip->addrs[NDIRECT+1]);
    a = (uint*)bp->data;
    for(i = 0; i < NINDIRECT; i++){
      uint base = NDIRECT + NINDIRECT + (uint)i*NINDIRECT;
      if(!a[i] || startbn >= base + NINDIRECT)
        continue;
      int inner_changed = 0;
      bp2 = bread(ip->dev, a[i]);
      a2 = (uint*)bp2->data;
      for(j = 0; j < NINDIRECT; j++){
        if(base + (uint)j >= startbn && a2[j]){
          bfree(ip->dev, a2[j]);
          a2[j] = 0;
          inner_changed = 1;
        }
      }
      if(inner_changed)
        log_write(bp2);
      brelse(bp2);
      if(startbn <= base){
        bfree(ip->dev, a[i]);
        a[i] = 0;
        outer_changed = 1;
      }
    }
    if(outer_changed)
      log_write(bp);
    brelse(bp);
    if(startbn <= NDIRECT + NINDIRECT){
      bfree(ip->dev, ip->addrs[NDIRECT+1]);
      ip->addrs[NDIRECT+1] = 0;
    }
  }
}

// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
static void
itrunc(struct inode *ip)
{
  itruncfrom(ip, 0);
  ip->size = 0;
  iupdate(ip);
}

// ftruncate(2)'s real backing logic (sys_ftruncate, kernel/sysfile.c).
// Shrinking frees blocks past the new size via itruncfrom(); growing
// just raises ip->size - the newly-in-range blocks don't exist yet,
// but that's fine: bmap() (called from readi()/writei() alike)
// allocates - and balloc() zeroes - any block the first time it's
// actually touched, direct or not, so a grow-then-read of the gap
// reads back zeros exactly as a real sparse-file hole would, without
// this port needing to track holes as a distinct concept at all.
// Caller must hold ip->lock.
int
itruncto(struct inode *ip, uint length)
{
  if(length > MAXFILE*BSIZE)
    return -1;
  if(length < ip->size){
    uint startbn = (length + BSIZE - 1) / BSIZE;
    itruncfrom(ip, startbn);
    // itruncfrom() only frees whole blocks at or past startbn - if
    // length isn't block-aligned, the block it falls inside (index
    // length/BSIZE, still very much alive: startbn is the block
    // *after* it) keeps its old bytes past length on disk. A later
    // grow back past length would otherwise resurface that stale
    // data as if it were part of the new hole, instead of the zeros a
    // real hole reads as everywhere else - the same information leak
    // real filesystems avoid by zeroing a truncated block's tail.
    if(length % BSIZE != 0){
      struct buf *bp = bread(ip->dev, bmap(ip, length/BSIZE));
      memset(bp->data + length%BSIZE, 0, BSIZE - length%BSIZE);
      log_write(bp);
      brelse(bp);
    }
  }
  ip->size = length;
  iupdate(ip);
  return 0;
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
  st->mode = ip->mode;
  st->uid = ip->uid;
  st->gid = ip->gid;
}

// Permission check: does a process with the given effective uid/gid have
// every bit in "want" (PERM_R|PERM_W|PERM_X) against ip's owner/group/
// other mode bits? Caller must hold ip->lock (every field read here is
// disk-backed and only valid once ilock()'d). Root (euid==0) bypasses
// every check, standard Unix semantics - checked first, before any mode
// bit is even read, so there's no bypass to remember at each call site.
int
permcheck(struct inode *ip, int euid, int egid, int want)
{
  int bits;

  if(euid == 0)
    return 0;
  if(ip->uid == euid)
    bits = (ip->mode >> 6) & 7;
  else if(ip->gid == egid)
    bits = (ip->mode >> 3) & 7;
  else
    bits = ip->mode & 7;
  return ((bits & want) == want) ? 0 : -1;
}

// Extends a bulk, cache-bypassing read (kernel/ide.c's ide_bulk_read())
// as far as it safely can from logical block bn, up to maxblocks whole
// blocks: stops at the first logical block whose on-disk address isn't
// exactly one past the previous one (bmap() making them non-contiguous
// - not guaranteed in general, though mkfs lays out a large file's
// blocks sequentially in practice, which is exactly the case this is
// for - a multi-megabyte asset like the wallpaper or a TTF font). Only
// actually bypasses the cache if bio_range_clean() (kernel/bio.c)
// confirms none of those on-disk blocks currently have an unwritten
// change sitting in a buffer; falls back to the plain per-block path
// below (returns 0) otherwise - correctness always wins over the
// optimization. See readi()'s own comment for why this exists at all.
static uint
readi_bulk_run(struct inode *ip, char *dst, uint bn, uint maxblocks)
{
  uint addr, first_addr, nb;

  first_addr = bmap(ip, bn);
  for(nb = 1; nb < maxblocks; nb++){
    addr = bmap(ip, bn + nb);
    if(addr != first_addr + nb)
      break;
  }
  if(!bio_range_clean(ip->dev, first_addr, nb))
    return 0;
  ide_bulk_read(first_addr, dst, nb);
  return nb;
}

//PAGEBREAK!
// Read data from inode.
// Caller must hold ip->lock.
//
// Tries readi_bulk_run() first whenever a whole aligned block or more
// remains - one memmove() for a whole run of contiguous, clean blocks
// instead of a bread()/memmove()/brelse() per block. That distinction
// matters: kernel/bio.c's bget() does an O(NBUF) linear scan (cache
// hit or miss) plus a lock acquire/release on *every* call, so a large
// sequential read - this kernel's own wallpaper/TTF font assets run
// into the thousands of blocks - paid that cost once per block for
// data it would only ever touch once, measured as several real seconds
// of delay before the login screen could finish loading its own
// background and fonts. Falls back to the original single-block path
// (unchanged below) for any misaligned remainder, a non-contiguous or
// dirty-covered stretch, or a file too small for the bulk path to ever
// trigger - metadata-heavy access patterns (repeatedly re-reading a
// directory or inode block) still go through the ordinary cache path
// exactly as before, since those still benefit from it.
int
readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    if(off % BSIZE == 0 && n - tot >= BSIZE){
      uint nb = readi_bulk_run(ip, dst, off / BSIZE, (n - tot) / BSIZE);
      if(nb > 0){
        m = nb * BSIZE;
        continue;
      }
    }
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(dst, bp->data + off%BSIZE, m);
    brelse(bp);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
// Caller must hold ip->lock.
int
writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(ip, src, n);
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE));
    m = min(n - tot, BSIZE - off%BSIZE);
    memmove(bp->data + off%BSIZE, src, m);
    log_write(bp);
    brelse(bp);
  }

  if(n > 0 && off > ip->size){
    ip->size = off;
    iupdate(ip);
  }
  return n;
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");

  return 0;
}

//PAGEBREAK!
// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    // Need search (x) permission on every directory a path descends
    // through - including the final one, to resolve the last component -
    // covering every syscall built on namei()/nameiparent() from this one
    // insertion point (see kernel/fs.c's permcheck() and include/fs.h's
    // PERM_X).
    if(permcheck(ip, myproc()->euid, myproc()->egid, PERM_X) < 0){
      iunlockput(ip);
      return 0;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
