// gui_proto: a small bespoke wire protocol over AF_UNIX SOCK_STREAM +
// SCM_RIGHTS - deliberately *not* a continuation of the real
// Wayland-wire opcodes GUI roadmap phase 4 built (bash/poc/wire.h,
// deleted in e149182 along with the rest of that phase's IPC layer).
// There is no upstream Wayland client this needs to interoperate
// with, so matching wl_seat/xdg_shell/etc.'s real spec would be
// unnecessary complexity for what's otherwise the same
// shm-buffer-passing mechanism ToaruOS's own bespoke "yutani"
// protocol already demonstrates is sufficient. See
// /Users/ranjith/.claude/plans/serene-coalescing-raven.md, Phase 7.
//
// Each message is one whole struct sent via a single sendmsg() call -
// kernel/socket.c's sockrecv()/socksend() are message-boundary-
// preserving (one sendmsg = one recvmsg, up to SOCKMSGSIZE=512 bytes,
// include/socket.h), so no length-prefix framing is needed on top;
// every message here also fits well under that 512-byte cap.
#ifndef GUI_PROTO_H
#define GUI_PROTO_H

#define GUI_SOCK_PATH "guisrv.sock"
#define GUI_TITLE_MAX 32

enum {
  GUI_MSG_CREATE_SURFACE = 1,  // client -> compositor
  GUI_MSG_SURFACE_CREATED = 2, // compositor -> client, shm fd rides via SCM_RIGHTS
  GUI_MSG_COMMIT = 3,          // client -> compositor
  GUI_MSG_DESTROY = 4,         // client -> compositor
  GUI_MSG_KEY_EVENT = 5,       // compositor -> client
  GUI_MSG_POINTER_EVENT = 6,   // compositor -> client
  GUI_MSG_FOCUS_EVENT = 7,     // compositor -> client
  // Taskbar protocol (ToaruOS-style minimize/restore): a client (gui/
  // desktop.c's bar) subscribes once, and thereafter gets an updated
  // GUI_MSG_TASK_LIST every time the compositor's window set,
  // minimized state, or focus changes; it answers a click on one of
  // its own task buttons with GUI_MSG_TASK_ACTION, identifying the
  // window by surface_id (stable across the window's whole lifetime,
  // unlike its index into the compositor's own windows[] array).
  GUI_MSG_TASK_SUBSCRIBE = 8, // client -> compositor, no payload beyond type
  GUI_MSG_TASK_LIST = 9,      // compositor -> subscribed client
  GUI_MSG_TASK_ACTION = 10,   // client -> compositor
  // Maximize/restore: the compositor decides the new content size and
  // creates the replacement shm block itself (mirrors
  // GUI_MSG_SURFACE_CREATED's own shm handoff - the client never calls
  // SYS_shm_create directly), re-keyed onto the *same* surface_id
  // rather than allocating a new window. The client must mmap the new
  // fd, munmap() its old surface, update its own w/h/pitch, and
  // recommit at the new size (gui/terminal.c additionally calls
  // libvterm's vterm_set_size() so the shell reflows instead of just
  // being stretched).
  GUI_MSG_RESIZE = 11,        // compositor -> client, new shm fd via SCM_RIGHTS
  // Lets a client (gui/login_gui.c) learn the framebuffer size right
  // after connecting, to compute a centered placement up front - no
  // window/shm allocation involved, unlike GUI_MSG_CREATE_SURFACE,
  // so a client no longer has to create-and-destroy a throwaway probe
  // surface just to read screen_w/screen_h off its reply.
  GUI_MSG_QUERY_SCREEN = 12,  // client -> compositor, no payload beyond type
  GUI_MSG_SCREEN_INFO = 13,   // compositor -> client
};

#define GUI_MAX_TASKS 8 // == MAXWIN (gui/compositor.c) - one taskbar
                        // entry per possible window

struct gui_task_entry {
  int surface_id;
  int minimized;
  int focused;
  char title[GUI_TITLE_MAX];
};

// GUI_WIN_BORDERLESS: compositor draws no title bar/border chrome for
// this window and uses its raw content size for hit-testing/redraw
// (not decor_w()/decor_h()) - for the desktop shell's top bar/icon and
// the login screen's own glass box, none of which should look like an
// ordinary app window (ToaruOS-style GUI rewrite).
#define GUI_WIN_BORDERLESS 0x1
// GUI_WIN_NO_FOCUS: compositor never gives this window keyboard focus
// (click-to-focus is skipped for it) - a separate concern from
// BORDERLESS: the desktop bar/icon are both borderless *and* must
// never steal keyboard focus from a real app window (e.g. the
// terminal), but the login screen is borderless and still very much
// needs focus to receive typed keystrokes at all.
#define GUI_WIN_NO_FOCUS 0x2
// GUI_WIN_DESKTOP_BG: only meaningful alongside GUI_WIN_NO_FOCUS.
// Without it, a NO_FOCUS window (gui/compositor.c's redraw_all()) is
// drawn in its own always-on-top pass, above every ordinary window -
// right for a taskbar/panel (gui/desktop.c's top bar), but wrong for
// a desktop icon: a normal window (e.g. a freshly launched terminal)
// that happens to overlap the icon's fixed screen position would
// always render underneath it. This flag moves a NO_FOCUS window into
// a separate bottom-most pass instead, drawn before ordinary windows
// - the same "lives on the desktop background, below every real
// window" layer icons occupy on any real desktop.
#define GUI_WIN_DESKTOP_BG 0x4

struct gui_msg_create_surface {
  int type;
  int w, h;
  // Requested top-left placement in screen coordinates, or (-1,-1) to
  // keep the compositor's existing auto-cascade behavior (unchanged
  // for ordinary app windows like the terminal). Lets a client that
  // knows the screen size (see screen_w/screen_h below) position
  // itself precisely - the login box centered, the desktop bar at
  // (0,0), the desktop icon at a fixed spot.
  int x, y;
  int flags;   // GUI_WIN_BORDERLESS, or 0
  char title[GUI_TITLE_MAX];
};

// Field layout mirrors include/fb.h's struct fb_info exactly - the
// client's gfx_surface uses the compositor's real framebuffer's own
// pixel format, so gfx_blit() is a raw per-scanline memcpy (see
// gfx.h's own comment).
struct gui_msg_surface_created {
  int type;
  int surface_id;
  unsigned int w, h, pitch;
  unsigned char bpp;
  unsigned char red_mask_size, red_field_pos;
  unsigned char green_mask_size, green_field_pos;
  unsigned char blue_mask_size, blue_field_pos;
  // Total framebuffer size, not this surface's own w/h - lets a
  // client compute its own centered/full-width placement (see
  // gui_msg_create_surface's x/y above) without a separate query.
  unsigned int screen_w, screen_h;
};

struct gui_msg_commit {
  int type;
  int surface_id;
};

struct gui_msg_destroy {
  int type;
  int surface_id;
};

struct gui_msg_query_screen {
  int type;
};

struct gui_msg_screen_info {
  int type;
  unsigned int screen_w, screen_h;
};

struct gui_msg_key_event {
  int type;
  int ch;      // ASCII byte from kbdgetc() (kernel/kbd.c) - press-only, no separate release event
};

struct gui_msg_pointer_event {
  int type;
  int x, y;    // window-relative
  int buttons; // bit 0 = left button
};

struct gui_msg_focus_event {
  int type;
  int focused; // 1 = gained focus, 0 = lost
};

struct gui_msg_task_subscribe {
  int type;
};

struct gui_msg_task_list {
  int type;
  int count; // valid entries in tasks[], 0..GUI_MAX_TASKS
  struct gui_task_entry tasks[GUI_MAX_TASKS];
};

struct gui_msg_task_action {
  int type;
  int surface_id;
};

// Field layout deliberately mirrors gui_msg_surface_created (same w/h/
// pitch fields, same "shm fd rides via SCM_RIGHTS" convention) - a
// resize is really "here is your surface_created reply again, for the
// window you already have" rather than a distinct shape.
struct gui_msg_resize {
  int type;
  int surface_id;
  unsigned int w, h, pitch;
};

// Largest of the above - sized so a single fixed-size recvmsg() buffer
// on either side can hold any message type without a preliminary
// "peek the type first" round trip.
union gui_msg {
  int type;
  struct gui_msg_create_surface create_surface;
  struct gui_msg_surface_created surface_created;
  struct gui_msg_commit commit;
  struct gui_msg_destroy destroy;
  struct gui_msg_key_event key_event;
  struct gui_msg_pointer_event pointer_event;
  struct gui_msg_focus_event focus_event;
  struct gui_msg_task_subscribe task_subscribe;
  struct gui_msg_task_list task_list;
  struct gui_msg_task_action task_action;
  struct gui_msg_resize resize;
  struct gui_msg_query_screen query_screen;
  struct gui_msg_screen_info screen_info;
};

#endif
