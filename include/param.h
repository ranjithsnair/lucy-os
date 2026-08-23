#define NPROC        64  // maximum number of processes
// Raised from 4096 (1 page) for the Limine port: kernel/trapasm.asm's
// frame-normalization fix (see its own comment) made a same-privilege
// (kernel-interrupting-kernel) trap return correctly instead of
// silently corrupting things - which in turn meant a timer tick
// landing *during* one of execve()'s several readi()/bread() calls now
// nests a full trap frame onto an already fairly deep kernel call
// chain, instead of never getting that far. Found the hard way (a
// VirtualBox VM): 1 page overflowed past the bottom of a process's
// kernel stack under exactly that nesting, corrupting whatever kalloc()
// page happened to sit below it - each fix just moved the crash to the
// next readi()/bread() call site deeper in, confirming it was stack
// exhaustion, not any one call site. kernel/proc.c's allocproc() uses a
// dedicated static array for kernel stacks now, not kalloc() (which
// only ever hands out one PGSIZE page at a time, no way to get several
// contiguous ones from it), so this can be page-unaligned in size.
#define KSTACKSIZE 16384
// USTACKPAGES: usable pages of user stack exec.c allocates (below a
// single guard page - see its own comment). A dynamically-linked
// program's real gnulib call chains run far deeper, with far bigger
// per-frame locals (vsnprintf's own on-stack ~256-byte buffer, times
// however many of nstrftime()/human_readable()/quote_name_buf()/...
// are nested when ls -l formats one line), than any of poc-os's own
// native, statically-linked user/*.c programs ever needed - 1 page
// (the original xv6 value) turned out to be too little the first
// time a real coreutils utility's deepest call chain (ls -l, of every
// utility shipped so far) actually got exercised: `rep stosl`
// zeroing vsnprintf's own local buffer faulted on a stack page past
// the single one mapped, well before any genuinely-unbounded
// recursion or runaway allocation was involved.
#define USTACKPAGES  16
#define NCPU          8  // maximum number of CPUs
// Raised from 16 for the ToaruOS-style GUI rewrite: a GUI client that
// double-forks to launch another GUI client (gui/desktop.c's
// launch_terminal(), which fork()+execve()s /usr/bin/terminal) has the
// new process inherit every fd the launcher had open (compositor
// socket(s), epoll instance, ...) on top of its own (another
// compositor socket, two pipes to bash, its own epoll instance) - 16
// turned out too tight (found via gui/terminal.c's own pipe() call
// failing with the 2nd of its two pipe() calls, once fd exhaustion was
// suspected and confirmed by tracing errno). kernel/epoll.c's
// EPOLL_MAXFDS is kept in sync with this, see its own comment.
#define NOFILE       32  // open files per process
#define NFILE       100  // open files per system
#define NINODE       50  // maximum number of active i-nodes
#define NDEV         10  // maximum major device number
#define ROOTDEV       1  // device number of file system root disk
#define MAXARG       32  // max exec arguments
#define MAXENVP      32  // max execve environment variables
#define MAXOPBLOCKS  10  // max # of blocks any FS op writes
#define LOGSIZE      (MAXOPBLOCKS*3)  // max data blocks in on-disk log
#define NBUF         (MAXOPBLOCKS*3)  // size of disk block cache
// mkfs/mkfs.c's balloc() writes the whole free-block bitmap as a
// single BSIZE-byte sector (one wsect() call, no indirection) - a
// hard ceiling of BSIZE*8 = 4096 blocks (2MB) on FSSIZE, not just a
// default; raising this past 4095 needs balloc() itself extended to
// span multiple bitmap sectors first. 4000 (2MB, up from the
// original 1000/500KB) is comfortably under that ceiling while
// giving real room to grow: as of this comment, UPROGS plus
// usr/lib/libc.so plus usr/bin/cat alone (see the Makefile) already
// used 884 of the original 1000 blocks, leaving usertests.c's own
// bigfile test (writetest1(), which deliberately writes a
// MAXFILE-sized ~70KB/140-block file to exercise indirect-block
// handling) no room to run without hitting balloc()'s "out of
// blocks" panic - confirmed by hitting exactly that panic once real
// coreutils utilities pushed usage that high.
// Raised from 4000 (2MB) for the ToaruOS-style GUI rewrite: the DejaVu
// TrueType fonts alone (usr/share/fonts/dejavu/*.ttf) total ~2.1MB, plus
// a wallpaper + icon raw asset - all comfortably past the old ceiling.
// mkfs/mkfs.c's balloc() now writes a real multi-sector bitmap (nbitmap
// = FSSIZE/(BSIZE*8)+1 sectors, matching kernel/fs.c's own BBLOCK(b,sb)
// addressing) instead of assuming everything fits in bmapstart's first
// sector, so the old "hard ceiling at BSIZE*8=4096 blocks no matter what
// FSSIZE says" limit this comment used to describe no longer applies.
#define FSSIZE       20000  // size of file system in blocks

