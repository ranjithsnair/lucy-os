// Linear-framebuffer info, filled in by kernel/vbe.c's vbeinit() from
// Limine's framebuffer response (kernel/limine.c) - the shape here
// predates Limine (it used to mirror a raw memory layout a real-mode
// VBE probe wrote by hand) but every consumer (kernel/sysproc.c's
// sys_mmap()/sys_ioctl() FRAMEBUFFER paths) still just wants these same
// fields, so the struct itself didn't need to change.
struct vbeinfo {
  uint magic;               // VBE_INFO_MAGIC iff a framebuffer was found
  uint phys_base;           // physical address of the linear framebuffer
  uint pitch;                // bytes per scanline
  uint xres;
  uint yres;
  uchar bpp;                  // bits per pixel (32)
  uchar red_mask_size;
  uchar red_field_pos;
  uchar green_mask_size;
  uchar green_field_pos;
  uchar blue_mask_size;
  uchar blue_field_pos;
  uchar reserved;              // pad to 28 bytes
} __attribute__((packed));

// Arbitrary nonzero sentinel - kernel/vbe.c's vbeinit() sets vbe.magic
// to this iff Limine actually reported a usable framebuffer, and clears
// it to 0 otherwise; kernel/sysproc.c's sys_mmap()/sys_ioctl() FRAMEBUFFER
// paths check it the same way.
#define VBE_INFO_MAGIC 0x31454256
