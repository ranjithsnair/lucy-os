// ACPI table walk, used only to enumerate CPUs' local APIC IDs.
//
// kernel/mp.c's mpinit() parses the legacy Intel MultiProcessor Table
// (the deprecated "_MP_"/"PCMP" structures) first, and that's still
// enough to find the local APIC's MMIO address, the I/O APIC's ID, and
// whether to mask the 8259 PIC via the IMCR - all of which QEMU's
// bundled SeaBIOS still fills in correctly for every CPU. What it does
// *not* reliably do, on any BIOS built in the last ~15-20 years real
// or virtual, is list every CPU as a processor entry in that legacy
// table: SeaBIOS (confirmed against the QEMU 11.0.2/SeaBIOS combination
// this kernel was tested against) writes exactly one MPPROC entry (the
// boot processor) regardless of -smp, and expects every other CPU to
// be discovered the modern way - via the Multiple APIC Description
// Table (MADT), one of the tables ACPI's RSDP/RSDT point at. So
// mpinit() calls acpiinit() (below) right after its own legacy-table
// parsing; if a MADT is found, its processor list replaces whatever
// cpus[]/ncpu the legacy table produced. If no MADT is found (no ACPI
// at all), acpiinit() leaves mpinit()'s legacy-table result alone -
// on real SMP-capable hardware that's never actually been seen, but it
// means a system with only the legacy table still boots single-CPU
// instead of panicking.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "x86.h"
#include "mmu.h"
#include "proc.h"

struct rsdp {                 // Root System Description Pointer
  uchar signature[8];           // "RSD PTR "
  uchar checksum;
  uchar oemid[6];
  uchar revision;
  uint  rsdtaddress;
  // ACPI 2.0+ adds a length/xsdtaddress/extendedchecksum/reserved tail
  // here, unused - see the "only RSDT" comment on acpiinit() below.
};

struct sdthdr {                // Common ACPI System Description Table header
  uchar signature[4];
  uint  length;                 // length of the whole table, header included
  uchar revision;
  uchar checksum;
  uchar oemid[6];
  uchar oemtableid[8];
  uint  oemrevision;
  uint  creatorid;
  uint  creatorrevision;
};

struct madt {                  // Multiple APIC Description Table
  struct sdthdr hdr;             // hdr.signature == "APIC"
  uint  lapicaddr;
  uint  flags;
  // followed by a stream of variable-length entries - see MADT_LAPIC/
  // MADT_IOAPIC below.
};

#define MADT_LAPIC   0          // Processor Local APIC entry
struct madtlapic {
  uchar type;                    // MADT_LAPIC
  uchar length;                  // 8
  uchar acpiid;
  uchar apicid;
  uint  flags;
    #define MADT_LAPIC_ENABLED 0x1
};

#define MADT_IOAPIC  1          // I/O APIC entry
struct madtioapic {
  uchar type;                    // MADT_IOAPIC
  uchar length;                  // 12
  uchar apicid;
  uchar reserved;
  uint  addr;
  uint  gsibase;
};

// ACPI tables (this one included) are validated the same way the MP
// spec's are (see kernel/mp.c's own sum()): every byte in the table,
// checksum byte included, must add up to 0 mod 256.
static uchar
sum(uchar *addr, int len)
{
  int i, s;

  s = 0;
  for(i = 0; i < len; i++)
    s += addr[i];
  return s;
}

// Look for the RSDP's "RSD PTR " signature in the len bytes at
// physical address a. Unlike kernel/mp.c's mpsearch1, the ACPI spec
// requires 16-byte alignment here, not sizeof(struct rsdp)-alignment.
static struct rsdp*
rsdpsearch1(uint a, int len)
{
  uchar *p, *e;

  p = HW_P2V(a);
  e = p + len;
  for(; p < e; p += 16)
    if(memcmp(p, "RSD PTR ", 8) == 0 && sum(p, 20) == 0)
      return (struct rsdp*)p;
  return 0;
}

// Search the same regions kernel/mp.c's mpsearch() does for "_MP_":
// the first KB of the EBDA, then the BIOS ROM area. Unlike mpsearch(),
// there's no "last KB of base memory" fallback - the ACPI spec doesn't
// define one, only the EBDA and BIOS-ROM locations.
static struct rsdp*
rsdpsearch(void)
{
  uchar *bda;
  uint p;
  struct rsdp *r;

  bda = (uchar*) HW_P2V(0x400);
  if((p = ((bda[0x0F]<<8) | bda[0x0E]) << 4)){
    if((r = rsdpsearch1(p, 1024)))
      return r;
  }
  return rsdpsearch1(0xE0000, 0x20000);
}

void
acpiinit(void)
{
  struct rsdp *rsdp;
  struct sdthdr *rsdt;
  struct madt *madt;
  uint *entries;
  int nentries, i;
  uchar *p, *e;
  uchar found[NCPU];
  int nfound;

  if((rsdp = rsdpsearch()) == 0 || rsdp->rsdtaddress == 0)
    return;   // No ACPI - keep whatever mpinit() already found.

  // Only the RSDT (32-bit table pointers) is read, never the ACPI
  // 2.0+ XSDT (64-bit pointers): QEMU/SeaBIOS always populates the
  // RSDT too, for exactly this backward-compatibility reason, so
  // there's no real-world case here that needs the XSDT.
  //
  // Every physical address read from here on gets kmapphys()'d before
  // it's dereferenced: unlike the legacy MP table (always well under
  // 1MB - see kernel/mp.c's mpsearch()), ACPI tables can land anywhere
  // in RAM, including above PHYSTOP - see kmapphys()'s own comment.
  kmapphys(rsdp->rsdtaddress, sizeof(struct sdthdr));
  rsdt = (struct sdthdr*) HW_P2V((uintp) rsdp->rsdtaddress);
  if(memcmp(rsdt->signature, "RSDT", 4) != 0)
    return;
  kmapphys(rsdp->rsdtaddress, rsdt->length);
  if(sum((uchar*)rsdt, rsdt->length) != 0)
    return;

  entries = (uint*)(rsdt + 1);
  nentries = (rsdt->length - sizeof(struct sdthdr)) / sizeof(uint);

  madt = 0;
  for(i = 0; i < nentries; i++){
    kmapphys(entries[i], sizeof(struct sdthdr));
    struct sdthdr *t = (struct sdthdr*) HW_P2V((uintp) entries[i]);
    if(memcmp(t->signature, "APIC", 4) != 0)
      continue;
    kmapphys(entries[i], t->length);
    if(sum((uchar*)t, t->length) == 0){
      madt = (struct madt*) t;
      break;
    }
  }
  if(madt == 0)
    return;

  // Collect into a local array first and only commit to the real
  // cpus[]/ncpu at the end, so a malformed MADT can't leave that
  // global state half-overwritten.
  nfound = 0;
  for(p = (uchar*)(madt+1), e = (uchar*)madt + madt->hdr.length; p+2 <= e && p[1] > 0; p += p[1]){
    if(p[0] == MADT_LAPIC && p + sizeof(struct madtlapic) <= e){
      struct madtlapic *la = (struct madtlapic*)p;
      if((la->flags & MADT_LAPIC_ENABLED) && nfound < NCPU)
        found[nfound++] = la->apicid;
    } else if(p[0] == MADT_IOAPIC && p + sizeof(struct madtioapic) <= e){
      ioapicid = ((struct madtioapic*)p)->apicid;
    }
  }
  if(nfound == 0)
    return;   // Nothing usable - keep mpinit()'s legacy-table result.

  ncpu = nfound;
  for(i = 0; i < nfound; i++)
    cpus[i].apicid = found[i];
  lapic = (uint*)(uintp) madt->lapicaddr;
}
