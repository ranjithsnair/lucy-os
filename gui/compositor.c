/* compositor: GUI roadmap phase 8 - the real yutani-equivalent daemon.
 * Owns the framebuffer and mouse devices directly (same open/mknod-
 * fallback/ioctl/mmap sequence as fbtest.c/guitest.c before it), puts
 * the console into raw mode so it becomes the sole reader of keyboard
 * input (kernel/kbd.c only ever feeds one global console stream - see
 * bash/poc/rawtest.c for the termios pattern this reuses), and listens
 * on GUI_SOCK_PATH for client connections (gui/libgui/gui_proto.h's
 * bespoke protocol).
 *
 * A fixed MAXWIN-sized window list with a back-to-front z-order array
 * (same scope-cut scale as every prior phase), server-side decorations
 * (title bar + border drawn by the compositor itself via libgui.so's
 * rasterizer - not a separate decorator client, a deliberate
 * simplification while this is being bootstrapped), click-to-focus
 * with raise-to-front, and title-bar drag-to-move. One epoll_wait()
 * loop (kernel/epoll.c, GUI roadmap phase 3/6) over the listening
 * socket, every connected client socket, the mouse fd, and the raw
 * console fd - full-frame redraw after every event, the same
 * "simplicity over efficiency" call guitest.c and every phase since
 * has already made (tiny screen, tiny window count, no real-time
 * constraint).
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>

#include "gfx.h"
#include "gui_proto.h"
#include "wire.h"

extern long syscall(long, ...);

// kernel/socket.c's socksend() (which wire_send() below eventually
// calls) genuinely *sleeps* the caller while a socket's outgoing queue
// is full (SOCKQLEN=3, include/socket.h) - fine for an ordinary client
// with one peer, but fatal here: this compositor is single-threaded,
// so blocking inside a send to any one client stalls its entire
// epoll_wait() loop, freezing every window, the mouse, and the
// keyboard until that specific client happens to drain its queue -
// indistinguishable from the whole GUI session hanging. sock_writable()
// is a non-blocking peek (SYS_sock_writable, kernel/socket.c's own
// sockwritable() - the same check kernel/epoll.c's EPOLLOUT support
// already uses internally) that must be checked before every send that
// isn't already known to be going to an idle, empty-queued connection.
static int
sock_writable(int fd)
{
	return syscall(SYS_sock_writable, fd) > 0;
}

// For the "fire and forget" async notifications (focus/pointer/key
// events, task-list updates) - all of these are superseded by
// whatever the *next* one carries, so silently dropping one when the
// target's queue is already full is harmless: at worst a client's
// view of e.g. the taskbar or its own focus state is briefly one
// update behind, which is far preferable to freezing the whole
// session over it.
static void
try_send(int fd, void *data, int len)
{
	if (sock_writable(fd))
		wire_send(fd, data, len, -1);
}

#define FRAMEBUFFER_MAJOR 2
#define MOUSE_MAJOR 3
#define FBIOGET_VSCREENINFO 1

struct fb_info {
	unsigned int xres, yres, pitch;
	unsigned char bpp;
	unsigned char red_mask_size, red_field_pos;
	unsigned char green_mask_size, green_field_pos;
	unsigned char blue_mask_size, blue_field_pos;
};

struct mousepkt {
	unsigned char buttons;
	signed char dx, dy;
};

#define MAXWIN 8
#define MAXEV (MAXWIN + 3)
#define TITLEBAR_H 20
#define BORDER 2
#define MOUSE_DRAIN_MAX 32 // cap on packets coalesced into one redraw - see
                            // its use in main()'s mfd branch

// Real cursor artwork: ToaruOS's own base/usr/share/cursor/normal.png,
// alpha-matted and cropped to its visible bounding box (17x23) via
// tools/genraw.py's "icon" mode (same RGBA raw pipeline as the desktop's
// terminal icon) - see gui/assets/images/cursor.raw. The crop was taken
// tight to the glyph's bbox, so the arrow's tip sits exactly at the raw
// image's own (0,0): drawing at (cursor_x, cursor_y) with no extra
// offset already puts the tip at the reported pointer position.
#define CURSOR_PATH "/usr/share/cursor.raw"
static struct gfx_image_rgba cursor_img; /* .pixels == 0 if the asset is missing */

// Fallback if cursor.raw didn't load: a crude 12x19 ASCII-art arrow,
// same top-left-is-the-tip hotspot convention as the real asset above.
// '#' black outline, '.' white fill, anything else (space, or the
// implicit '\0' padding on rows shorter than CURSOR_W) transparent.
// Fixed-size rows rather than `const char *` so a row that's too long
// is a compile error instead of an out-of-bounds read at draw time.
#define CURSOR_W 12
#define CURSOR_H 19
static const char cursor_bitmap[CURSOR_H][CURSOR_W + 1] = {
	"#",
	"##",
	"#.#",
	"#..#",
	"#...#",
	"#....#",
	"#.....#",
	"#......#",
	"#.......#",
	"#........#",
	"#.....#####",
	"#..#..#",
	"#.# #..#",
	"##   #..#",
	"#     #..#",
	"      #..#",
	"      #..#",
	"       ##",
	"",
};

static void
draw_cursor(struct gfx_surface *s, int x, int y)
{
	int row, col;

	if (cursor_img.pixels) {
		gfx_blit_alpha(s, x, y, &cursor_img, 0, 0, (int)cursor_img.w, (int)cursor_img.h);
		return;
	}

	for (row = 0; row < CURSOR_H; row++) {
		for (col = 0; col < CURSOR_W; col++) {
			char c = cursor_bitmap[row][col];
			int px, py;

			if (c != '#' && c != '.')
				continue;
			px = x + col;
			py = y + row;
			if (px < 0 || px >= (int)s->w || py < 0 || py >= (int)s->h)
				continue;
			*(unsigned int *)(s->pixels + (unsigned long)py * s->pitch + (unsigned long)px * 4) =
				gfx_pack(s, c == '#' ? 0x000000 : 0xFFFFFF);
		}
	}
}

// Flat-fill approximation of ToaruOS's default "Fancy" decoration theme
// (toaruos/lib/decor-fancy.c's ACTIVE_COLOR/INACTIVE_COLOR/
// BORDER_COLOR) - real hardware/theme sprite-sheets are out of scope
// here, but the same three colors carry the same look.
#define COLOR_BG        0x101820   // used only if the wallpaper asset is missing
#define COLOR_BORDER    0x3E3E3E
#define COLOR_TITLE_FOC 0xE2E2E2
#define COLOR_TITLE_UNF 0x939393
#define COLOR_TITLE_TXT 0x202020

// Button geometry: right-to-left slot order matches ToaruOS's own
// lib/decor-fancy.c layout (its BUTTON_OFFSET constants put close
// nearest the edge, then maximize, then minimize) - slot 0 = close,
// 1 = maximize, 2 = minimize.
#define BTN_SIZE   16
#define BTN_MARGIN 4
#define BTN_GAP    6

// Reserves this much of the top of the screen for gui/desktop.c's own
// top bar, so a maximized window's content never grows into it -
// mirrors that file's own PANEL_H. Not shared via a common header:
// this codebase already duplicates small ABI-shaped constants across
// components rather than introduce one for two callers (see struct
// fb_info's own precedent, just below).
#define TOPBAR_RESERVED_H 27

// Default cascade origin for a new window that didn't request a
// specific (x,y) - see handle_create_surface() below. Kept clear of
// gui/desktop.c's own fixed desktop-icon rect (ICON_X=20, ICON_Y=40,
// ICON_W=ICON_H=72 there) for the same reason TOPBAR_RESERVED_H above
// keeps clear of its top bar: GUI_WIN_DESKTOP_BG (gui_proto.h) already
// makes the icon render behind any ordinary window that happens to
// overlap it, but a brand-new window (e.g. gui/terminal.c, launched by
// double-clicking that very icon) starting out on top of the thing the
// user just clicked reads as broken even when the compositing order is
// technically correct - simplest fix is to just not cascade into that
// corner at all.
#define CASCADE_BASE_X 140
#define CASCADE_BASE_Y 60

#define WALLPAPER_PATH "/usr/share/wallpaper.raw"

struct window {
	int inuse;
	int fd;
	int surface_id;
	int shmfd;                /* kept open for the surface's lifetime -
	                            * see gui/libgui/client.c's own comment:
	                            * kernel/shm.c only refcounts open fds,
	                            * not active mmap() mappings, so closing
	                            * this early frees the pages while still
	                            * mapped. */
	struct gfx_surface surf; /* client's content area, mmap'd shm */
	int x, y;                /* top-left of the DECORATION (title bar) */
	int flags;                /* GUI_WIN_* from gui_proto.h */
	char title[GUI_TITLE_MAX];
	int committed;
	int minimized;            /* hidden from both redraw_all() and
	                            * window_at() but the client stays
	                            * connected - see GUI_MSG_TASK_ACTION */
	int maximized;
	int restore_x, restore_y;               /* pre-maximize decoration position */
	int restore_content_w, restore_content_h; /* pre-maximize content size */
};

static struct window windows[MAXWIN];
static int zorder[MAXWIN]; /* indices into windows[], back(0) to front(nz-1) */
static int nz;
static int focus_idx = -1;
static struct gfx_surface fbsurf;
// System-RAM shadow of fbsurf: redraw_all() composites into this;
// flush_dirty() below then copies only the changed sub-rect out to the
// real (mmap'd) framebuffer. Without this intermediate buffer, every
// gfx_fill_rect()/gfx_blit() call in redraw_all() would land on-screen
// individually - visibly flickering (wallpaper, then each window, then
// the cursor, drawn as separate frames) since nothing stops the
// display from scanning out a half-composited frame.
static struct gfx_surface backbuf;
static struct gfx_surface wallpaper; /* .pixels == 0 if the asset is missing */
static int cursor_x, cursor_y;
static int next_surface_id = 1;
// The one client (gui/desktop.c's bar) that asked for GUI_MSG_TASK_LIST
// updates via GUI_MSG_TASK_SUBSCRIBE - -1 if none has (yet).
static int taskbar_fd = -1;

// Dirty-rect tracking for flush_dirty() below: redraw_all() still
// recomposites the *entire* backbuf every event (RAM-to-RAM, cheap),
// but only the sub-rect actually touched since the last flush needs to
// go back out to the real (now write-combined, but still far slower
// than RAM) framebuffer. mark_dirty() callers only need to give a safe
// superset of what changed - a rect that's bigger than necessary just
// costs a few extra pixels; a rect that's too small would leave a
// stale on-screen artifact, so every call site below either tracks the
// exact old+new region involved or falls back to mark_dirty_full().
struct rect { int x0, y0, x1, y1; };
static struct rect dirty;
static int dirty_valid;

static void
mark_dirty(int x0, int y0, int x1, int y1)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > (int)fbsurf.w) x1 = (int)fbsurf.w;
	if (y1 > (int)fbsurf.h) y1 = (int)fbsurf.h;
	if (x0 >= x1 || y0 >= y1)
		return;

	if (!dirty_valid) {
		dirty.x0 = x0; dirty.y0 = y0; dirty.x1 = x1; dirty.y1 = y1;
		dirty_valid = 1;
		return;
	}
	if (x0 < dirty.x0) dirty.x0 = x0;
	if (y0 < dirty.y0) dirty.y0 = y0;
	if (x1 > dirty.x1) dirty.x1 = x1;
	if (y1 > dirty.y1) dirty.y1 = y1;
}

static void
mark_dirty_full(void)
{
	mark_dirty(0, 0, (int)fbsurf.w, (int)fbsurf.h);
}

// True if at least one window currently has real, committed content
// to show - not just "a window exists" (a freshly-created, not-yet-
// committed window contributes nothing visible, and a minimized one
// is deliberately not drawn either). Used by flush_dirty() below to
// refuse to push a frame that would only show bare wallpaper: that
// can happen momentarily at any window-count transition through zero
// - compositor startup (before gui/login_gui.c's first commit), the
// login->desktop handoff (login_gui.c calls gui_destroy() on its own
// window *before* execve()-ing into gui/desktop.c, so there's a real
// gap where zero windows exist until gui/desktop.c's icon/bar connect
// and commit), or even later if every window is ever closed at once -
// and a stray mouse-move event landing in any such gap used to be
// enough to trigger a redraw_all()+flush_dirty() that flushed the
// compositor's own wallpaper background to the screen, producing
// exactly the "boot splash/login box flashes to bare wallpaper before
// the next real screen is ready" symptom this avoids. Checked fresh on
// every flush (not just once) since this gap can recur, not just
// happen once at startup.
static int
has_committed_content(void)
{
	int i;

	for (i = 0; i < MAXWIN; i++)
		if (windows[i].inuse && windows[i].committed && !windows[i].minimized)
			return 1;
	return 0;
}

static void
flush_dirty(void)
{
	if (!dirty_valid)
		return;
	if (!has_committed_content())
		return;
	gfx_blit(&fbsurf, dirty.x0, dirty.y0, &backbuf, dirty.x0, dirty.y0,
	         dirty.x1 - dirty.x0, dirty.y1 - dirty.y0);
	dirty_valid = 0;
}

static int
decor_w(struct window *w)
{
	if (w->flags & GUI_WIN_BORDERLESS)
		return (int)w->surf.w;
	return (int)w->surf.w + 2 * BORDER;
}

static int
decor_h(struct window *w)
{
	if (w->flags & GUI_WIN_BORDERLESS)
		return (int)w->surf.h;
	return (int)w->surf.h + TITLEBAR_H + BORDER;
}

// The three titlebar buttons live in the top-right corner, in ToaruOS's
// own right-to-left order (see BTN_SIZE's own comment) - only decorated
// (non-borderless) windows have a titlebar to put them in, so borderless
// windows (desktop bar/icon, the login box) never get any.
static int
btn_x(struct window *w, int slot)
{
	return w->x + decor_w(w) - BTN_MARGIN - (slot + 1) * BTN_SIZE - slot * BTN_GAP;
}

static int
btn_y(struct window *w)
{
	return w->y + (TITLEBAR_H - BTN_SIZE) / 2;
}

static int
in_btn(struct window *w, int slot, int px, int py)
{
	int bx = btn_x(w, slot), by = btn_y(w);

	return !(w->flags & GUI_WIN_BORDERLESS) &&
	       px >= bx && px < bx + BTN_SIZE &&
	       py >= by && py < by + BTN_SIZE;
}

static int in_close_btn(struct window *w, int px, int py) { return in_btn(w, 0, px, py); }
static int in_max_btn(struct window *w, int px, int py) { return in_btn(w, 1, px, py); }
static int in_min_btn(struct window *w, int px, int py) { return in_btn(w, 2, px, py); }

// Pushes the current window set to whichever client subscribed via
// GUI_MSG_TASK_SUBSCRIBE (gui/desktop.c's bar) - called from every
// point that changes what a taskbar would need to show: window
// creation/removal, focus changes, and minimize/restore. NO_FOCUS
// windows (the bar/icon themselves) are never real "tasks" and are
// excluded, same filter handle_create_surface() already uses to decide
// which windows are even focusable.
static void
send_task_list(void)
{
	union gui_msg msg;
	int i, n = 0;

	if (taskbar_fd < 0)
		return;

	memset(&msg, 0, sizeof(msg));
	msg.task_list.type = GUI_MSG_TASK_LIST;
	for (i = 0; i < MAXWIN && n < GUI_MAX_TASKS; i++) {
		if (!windows[i].inuse || (windows[i].flags & GUI_WIN_NO_FOCUS))
			continue;
		msg.task_list.tasks[n].surface_id = windows[i].surface_id;
		msg.task_list.tasks[n].minimized = windows[i].minimized;
		msg.task_list.tasks[n].focused = (i == focus_idx);
		memcpy(msg.task_list.tasks[n].title, windows[i].title, GUI_TITLE_MAX);
		n++;
	}
	msg.task_list.count = n;
	try_send(taskbar_fd, &msg, sizeof(msg));
}

static void
zorder_remove(int idx)
{
	int i, j;

	for (i = 0; i < nz; i++) {
		if (zorder[i] == idx) {
			for (j = i; j < nz - 1; j++)
				zorder[j] = zorder[j + 1];
			nz--;
			return;
		}
	}
}

static void
zorder_raise(int idx)
{
	zorder_remove(idx);
	zorder[nz++] = idx;
}

static void
set_focus(int idx)
{
	struct gui_msg_focus_event ev;

	if (focus_idx == idx)
		return;
	if (focus_idx >= 0 && windows[focus_idx].inuse) {
		ev.type = GUI_MSG_FOCUS_EVENT;
		ev.focused = 0;
		try_send(windows[focus_idx].fd, &ev, sizeof(ev));
	}
	focus_idx = idx;
	if (focus_idx >= 0) {
		ev.type = GUI_MSG_FOCUS_EVENT;
		ev.focused = 1;
		try_send(windows[focus_idx].fd, &ev, sizeof(ev));
	}
	send_task_list();
}

static void
remove_window(int epfd, int idx)
{
	struct window *w = &windows[idx];

	if (w->fd == taskbar_fd)
		taskbar_fd = -1;
	epoll_ctl(epfd, EPOLL_CTL_DEL, w->fd, 0);
	close(w->fd);
	close(w->shmfd);
	zorder_remove(idx);
	if (focus_idx == idx)
		focus_idx = nz > 0 ? zorder[nz - 1] : -1;
	w->inuse = 0;
	w->minimized = 0;
	w->maximized = 0;
	send_task_list();
}

static int
window_at(int px, int py, int *in_titlebar)
{
	int i, idx;
	struct window *w;

	for (i = nz - 1; i >= 0; i--) {
		idx = zorder[i];
		w = &windows[idx];
		if (w->minimized)
			continue;
		if (px >= w->x && px < w->x + decor_w(w) &&
		    py >= w->y && py < w->y + decor_h(w)) {
			*in_titlebar = !(w->flags & GUI_WIN_BORDERLESS) &&
			               py < w->y + TITLEBAR_H;
			return idx;
		}
	}
	return -1;
}

// Draws one decorated window's titlebar buttons: simple monochrome
// glyphs in the titlebar's own text color (matching ToaruOS's
// lib/decor-fancy.c, which paints its close/maximize/minimize sprites
// tinted to ACTIVE_COLOR/INACTIVE_COLOR rather than using a colored
// button background) rather than the single filled-red-square close
// button this used to draw. No hover-highlight state: ToaruOS's
// decorator tracks continuous pointer-motion hover per button, but
// this compositor only ever reacts to clicks - adding continuous
// decoration-hover tracking is out of scope here.
static void
draw_titlebar_buttons(struct window *w, unsigned int color)
{
	int bx, by;

	bx = btn_x(w, 0); by = btn_y(w);
	gfx_draw_string(&backbuf, bx + 4, by, "x", color);

	bx = btn_x(w, 1); by = btn_y(w);
	{
		int sx0 = bx + 3, sy0 = by + 3, sx1 = bx + BTN_SIZE - 3, sy1 = by + BTN_SIZE - 3;

		gfx_fill_rect(&backbuf, sx0, sy0, sx1, sy0 + 1, color);   /* top */
		gfx_fill_rect(&backbuf, sx0, sy1 - 1, sx1, sy1, color);   /* bottom */
		gfx_fill_rect(&backbuf, sx0, sy0, sx0 + 1, sy1, color);   /* left */
		gfx_fill_rect(&backbuf, sx1 - 1, sy0, sx1, sy1, color);   /* right */
	}

	bx = btn_x(w, 2); by = btn_y(w);
	gfx_fill_rect(&backbuf, bx + 3, by + BTN_SIZE - 5, bx + BTN_SIZE - 3, by + BTN_SIZE - 3, color);
}

static void
redraw_all(void)
{
	int i, idx;
	struct window *w;

	if (wallpaper.pixels)
		gfx_blit(&backbuf, 0, 0, &wallpaper, 0, 0, (int)wallpaper.w, (int)wallpaper.h);
	else
		gfx_fill_rect(&backbuf, 0, 0, (int)backbuf.w, (int)backbuf.h, COLOR_BG);

	// GUI_WIN_DESKTOP_BG windows (the desktop icon) sit on the
	// background layer, below every ordinary window - drawn first so
	// any normal window overlapping them (e.g. a freshly launched
	// terminal cascaded right over the icon's fixed position) covers
	// them, the same as any real desktop's icon-vs-window stacking.
	for (i = 0; i < nz; i++) {
		idx = zorder[i];
		w = &windows[idx];
		if (w->minimized || !(w->flags & GUI_WIN_DESKTOP_BG))
			continue;
		if (w->committed)
			gfx_blit(&backbuf, w->x, w->y, &w->surf, 0, 0, (int)w->surf.w, (int)w->surf.h);
	}

	for (i = 0; i < nz; i++) {
		idx = zorder[i];
		w = &windows[idx];
		// NO_FOCUS windows (the desktop bar/icon) are drawn outside
		// ordinary z-order, in either the pass just above (icon) or
		// the second, always-on-top pass below (bar) instead -
		// otherwise a maximized window (which fills everything below
		// TOPBAR_RESERVED_H, but is still just an ordinary z-order
		// entry) could paint right over the panel the moment it's
		// raised above it.
		if (w->minimized || (w->flags & GUI_WIN_NO_FOCUS))
			continue;
		if (w->flags & GUI_WIN_BORDERLESS) {
			if (w->committed)
				gfx_blit(&backbuf, w->x, w->y, &w->surf, 0, 0,
				         (int)w->surf.w, (int)w->surf.h);
			continue;
		}
		gfx_fill_rect(&backbuf, w->x, w->y, w->x + decor_w(w), w->y + decor_h(w), COLOR_BORDER);
		gfx_fill_rect(&backbuf, w->x, w->y, w->x + decor_w(w), w->y + TITLEBAR_H,
		              idx == focus_idx ? COLOR_TITLE_FOC : COLOR_TITLE_UNF);
		gfx_draw_string(&backbuf, w->x + 4, w->y + 4, w->title, COLOR_TITLE_TXT);
		// Buttons are drawn in the same color as the title text itself
		// (which likewise doesn't vary by focus state here) rather than
		// ToaruOS's own black-titlebar-background convention - this
		// titlebar fills with a light ACTIVE/INACTIVE_COLOR instead and
		// needs a dark foreground for contrast, the opposite pairing.
		draw_titlebar_buttons(w, COLOR_TITLE_TXT);
		if (w->committed)
			gfx_blit(&backbuf, w->x + BORDER, w->y + TITLEBAR_H,
			         &w->surf, 0, 0, (int)w->surf.w, (int)w->surf.h);
	}

	for (i = 0; i < nz; i++) {
		idx = zorder[i];
		w = &windows[idx];
		if (w->minimized || !(w->flags & GUI_WIN_NO_FOCUS) || (w->flags & GUI_WIN_DESKTOP_BG))
			continue;
		if (w->committed)
			gfx_blit(&backbuf, w->x, w->y, &w->surf, 0, 0, (int)w->surf.w, (int)w->surf.h);
	}

	draw_cursor(&backbuf, cursor_x, cursor_y);
	// Recompositing above is always full-screen (cheap: backbuf is
	// plain RAM) - callers decide how much of the result actually needs
	// to reach the real framebuffer via mark_dirty()/flush_dirty().
}

static int wallpaper_pending = 1;

// The wallpaper is a multi-megabyte raw dump (see gfx_load_raw()'s own
// per-pixel format-conversion loop) - loading it eagerly at startup
// used to block this process from ever reaching epoll_wait(), which
// meant the boot splash just sat frozen on screen for the whole load
// before the first client (gui/login_gui.c) could even connect.
// Instead, wallpaper.pixels starts (and stays) 0 - redraw_all() falls
// back to COLOR_BG - so the very first client's window can appear on
// a flat desktop almost immediately; this is called once, right after
// that first commit already reached the real framebuffer, to load the
// real wallpaper in the background and swap it in behind whatever's
// already on screen. wallpaper_pending gates it to a single attempt
// regardless of how many commits arrive afterward.
static void
load_wallpaper_deferred(void)
{
	if (!wallpaper_pending)
		return;
	wallpaper_pending = 0;

	// wallpaper.pixels stays 0 (redraw_all() falls back to COLOR_BG) if
	// the asset is missing or doesn't match the real resolution - no
	// general image scaler, see tools/genraw.py's own comment.
	gfx_load_raw(&wallpaper, WALLPAPER_PATH, &fbsurf);
	if (wallpaper.pixels && (wallpaper.w != fbsurf.w || wallpaper.h != fbsurf.h)) {
		free(wallpaper.pixels);
		wallpaper.pixels = 0;
	}
	if (wallpaper.pixels) {
		redraw_all();
		mark_dirty_full();
		flush_dirty();
	}
}

static void
handle_create_surface(int epfd, int fd, struct gui_msg_create_surface *req)
{
	int idx, shmfd;
	union gui_msg reply;
	static int cascade;

	for (idx = 0; idx < MAXWIN; idx++)
		if (!windows[idx].inuse)
			break;
	if (idx == MAXWIN || req->w <= 0 || req->h <= 0) {
		close(fd);
		return;
	}

	shmfd = syscall(SYS_shm_create, (unsigned int)(req->w * 4 * req->h));
	if (shmfd < 0) {
		close(fd);
		return;
	}

	memset(&reply, 0, sizeof(reply));
	reply.surface_created.type = GUI_MSG_SURFACE_CREATED;
	reply.surface_created.surface_id = next_surface_id++;
	reply.surface_created.w = (unsigned int)req->w;
	reply.surface_created.h = (unsigned int)req->h;
	reply.surface_created.pitch = (unsigned int)req->w * 4;
	reply.surface_created.bpp = 32;
	reply.surface_created.red_mask_size = fbsurf.red_mask_size;
	reply.surface_created.red_field_pos = fbsurf.red_field_pos;
	reply.surface_created.green_mask_size = fbsurf.green_mask_size;
	reply.surface_created.green_field_pos = fbsurf.green_field_pos;
	reply.surface_created.blue_mask_size = fbsurf.blue_mask_size;
	reply.surface_created.blue_field_pos = fbsurf.blue_field_pos;
	reply.surface_created.screen_w = fbsurf.w;
	reply.surface_created.screen_h = fbsurf.h;

	if (wire_send(fd, &reply, sizeof(reply), shmfd) != (int)sizeof(reply)) {
		close(shmfd);
		close(fd);
		return;
	}

	windows[idx].surf.pixels = mmap(0, (unsigned long)req->w * 4 * (unsigned long)req->h,
	                                 PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
	if (windows[idx].surf.pixels == MAP_FAILED) {
		close(shmfd);
		close(fd);
		return;
	}
	windows[idx].inuse = 1;
	windows[idx].fd = fd;
	windows[idx].shmfd = shmfd;
	windows[idx].surface_id = reply.surface_created.surface_id;
	windows[idx].surf.w = (unsigned int)req->w;
	windows[idx].surf.h = (unsigned int)req->h;
	windows[idx].surf.pitch = (unsigned int)req->w * 4;
	windows[idx].surf.red_mask_size = fbsurf.red_mask_size;
	windows[idx].surf.red_field_pos = fbsurf.red_field_pos;
	windows[idx].surf.green_mask_size = fbsurf.green_mask_size;
	windows[idx].surf.green_field_pos = fbsurf.green_field_pos;
	windows[idx].surf.blue_mask_size = fbsurf.blue_mask_size;
	windows[idx].surf.blue_field_pos = fbsurf.blue_field_pos;
	memcpy(windows[idx].title, req->title, sizeof(windows[idx].title));
	windows[idx].title[GUI_TITLE_MAX - 1] = 0;
	windows[idx].committed = 0;
	windows[idx].flags = req->flags;
	if (req->x == -1 && req->y == -1) {
		windows[idx].x = CASCADE_BASE_X + 30 * cascade;
		windows[idx].y = CASCADE_BASE_Y + 30 * cascade;
		cascade = (cascade + 1) % 6;
	} else {
		windows[idx].x = req->x;
		windows[idx].y = req->y;
	}

	zorder[nz++] = idx;
	// NO_FOCUS windows (desktop bar/icon) never steal keyboard focus -
	// they only care about pointer clicks, and the whole point is to
	// leave a real app window (e.g. the terminal) as the keyboard target.
	//
	// No explicit send_task_list() call here: for a focusable window,
	// set_focus() below always actually changes focus_idx (a brand new
	// window can never already be the current focus) and already sends
	// one itself; for a NO_FOCUS window, it's excluded from the task
	// list anyway (send_task_list()'s own filter), so no update is
	// needed either way. A second explicit send here used to double up
	// with set_focus()'s own send on every single window creation -
	// harmless alone, but stacked with a client that's slow to drain
	// its own incoming queue (SOCKQLEN=3, include/socket.h - e.g. a
	// second terminal window still busy with its own startup/font
	// loading), the extra send was sometimes the one that tipped a
	// client's queue over the edge and wedged this wire_send() - see
	// the mouse-burst coalescing comment elsewhere in this file for the
	// same underlying failure mode.
	if (!(req->flags & GUI_WIN_NO_FOCUS))
		set_focus(idx);
}

// Maximize/restore: allocates a *new* shm block of the given content
// size, hands it to the window's client via GUI_MSG_RESIZE (same
// "shm fd rides via SCM_RIGHTS" convention as GUI_MSG_SURFACE_CREATED,
// just re-keyed onto this window's existing surface_id instead of
// allocating a new window slot), then drops the old mapping on this
// (the compositor's) side. Returns -1 (leaving the window as it was)
// on any allocation failure, 0 on success. windows[idx].committed is
// deliberately cleared - the old content is the wrong size/stale the
// moment this returns, and must not be blitted again until the client
// answers with a fresh GUI_MSG_COMMIT at the new size.
static int
resize_window_content(int idx, int new_w, int new_h)
{
	struct window *w = &windows[idx];
	int newfd;
	void *newpix;
	union gui_msg msg;

	newfd = syscall(SYS_shm_create, (unsigned int)(new_w * 4 * new_h));
	if (newfd < 0)
		return -1;
	newpix = mmap(0, (unsigned long)new_w * 4 * (unsigned long)new_h,
	              PROT_READ | PROT_WRITE, MAP_SHARED, newfd, 0);
	if (newpix == MAP_FAILED) {
		close(newfd);
		return -1;
	}

	memset(&msg, 0, sizeof(msg));
	msg.resize.type = GUI_MSG_RESIZE;
	msg.resize.surface_id = w->surface_id;
	msg.resize.w = (unsigned int)new_w;
	msg.resize.h = (unsigned int)new_h;
	msg.resize.pitch = (unsigned int)new_w * 4;
	// Unlike the "fire and forget" events try_send() drops when a
	// client's queue is full (see its own comment), this one carries
	// the new shm fd and changes what size the client (and this
	// compositor's own w->surf right below) agree the window's content
	// is - silently dropping it would desync the two, not just show
	// slightly-stale state. So: check first and fail the whole
	// maximize/restore (caller leaves the window as it was) rather than
	// either blocking the compositor or sending something that would
	// corrupt that agreement.
	if (!sock_writable(w->fd) ||
	    wire_send(w->fd, &msg, sizeof(msg), newfd) != (int)sizeof(msg)) {
		munmap(newpix, (unsigned long)new_w * 4 * (unsigned long)new_h);
		close(newfd);
		return -1;
	}

	munmap(w->surf.pixels, (unsigned long)w->surf.pitch * w->surf.h);
	close(w->shmfd);
	w->shmfd = newfd;
	w->surf.pixels = newpix;
	w->surf.w = (unsigned int)new_w;
	w->surf.h = (unsigned int)new_h;
	w->surf.pitch = (unsigned int)new_w * 4;
	w->committed = 0;
	return 0;
}

int
main(void)
{
	int fbfd, mfd, lfd, epfd;
	struct fb_info fi;
	struct sockaddr_un addr;
	struct epoll_event ev, events[MAXEV];
	struct termios orig, raw;
	int dragging = -1, drag_off_x = 0, drag_off_y = 0;
	int i;

	fbfd = open("framebuffer", O_RDWR);
	if (fbfd < 0) {
		syscall(SYS_mknod, "framebuffer", FRAMEBUFFER_MAJOR, 0);
		fbfd = open("framebuffer", O_RDWR);
	}
	if (fbfd < 0 || ioctl(fbfd, FBIOGET_VSCREENINFO, &fi) < 0) {
		printf("compositor: no usable framebuffer\n");
		return 1;
	}
	fbsurf.pixels = mmap(0, fi.pitch * fi.yres, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
	if (fbsurf.pixels == MAP_FAILED) {
		printf("compositor: framebuffer mmap failed\n");
		return 1;
	}
	fbsurf.w = fi.xres;
	fbsurf.h = fi.yres;
	fbsurf.pitch = fi.pitch;
	fbsurf.red_mask_size = fi.red_mask_size;
	fbsurf.red_field_pos = fi.red_field_pos;
	fbsurf.green_mask_size = fi.green_mask_size;
	fbsurf.green_field_pos = fi.green_field_pos;
	fbsurf.blue_mask_size = fi.blue_mask_size;
	fbsurf.blue_field_pos = fi.blue_field_pos;

	backbuf = fbsurf;
	backbuf.pitch = fbsurf.w * 4; /* tightly packed - gfx_blit() copies scanline-by-scanline, so a
	                               * different pitch than fbsurf's is fine */
	backbuf.pixels = malloc((unsigned long)backbuf.pitch * fbsurf.h);
	if (!backbuf.pixels) {
		printf("compositor: backbuf alloc failed\n");
		return 1;
	}

	mfd = open("mouse", O_RDONLY);
	if (mfd < 0) {
		syscall(SYS_mknod, "mouse", MOUSE_MAJOR, 0);
		mfd = open("mouse", O_RDONLY);
	}
	if (mfd < 0) {
		printf("compositor: no usable mouse\n");
		return 1;
	}

	if (tcgetattr(0, &orig) < 0) {
		printf("compositor: tcgetattr failed\n");
		return 1;
	}
	raw = orig;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(0, TCSANOW, &raw) < 0) {
		printf("compositor: tcsetattr(raw) failed\n");
		return 1;
	}

	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0) {
		printf("compositor: socket failed\n");
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, GUI_SOCK_PATH);
	if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(lfd, 8) < 0) {
		printf("compositor: bind/listen failed\n");
		tcsetattr(0, TCSANOW, &orig);
		return 1;
	}

	epfd = epoll_create1(0);
	if (epfd < 0) {
		printf("compositor: epoll_create1 failed\n");
		return 1;
	}
	ev.events = EPOLLIN;
	ev.data.fd = lfd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
	ev.data.fd = mfd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, mfd, &ev);
	ev.data.fd = 0;
	epoll_ctl(epfd, EPOLL_CTL_ADD, 0, &ev);

	cursor_x = (int)fbsurf.w / 2;
	cursor_y = (int)fbsurf.h / 2;
	// cursor_img.pixels stays 0 (draw_cursor() falls back to the ASCII
	// arrow) if the asset is missing - small enough to load eagerly,
	// unlike the wallpaper (see load_wallpaper_deferred() below).
	gfx_load_raw_rgba(&cursor_img, CURSOR_PATH);
	// Deliberately no redraw_all()/flush here: bash/poc/dinit.c runs
	// gui/bootsplash.c before this process ever starts, and that
	// splash image is still sitting in the real framebuffer right now
	// (this mmap() didn't touch it) - leave it alone until the first
	// real client (gui/login_gui.c) actually commits something, so the
	// splash transitions directly into the login box with no
	// intermediate bare-wallpaper frame.
	printf("compositor: listening on %s\n", GUI_SOCK_PATH);

	for (;;) {
		int n = epoll_wait(epfd, events, MAXEV, -1);

		if (n < 0)
			continue;

		for (i = 0; i < n; i++) {
			int fd = events[i].data.fd;

			if (fd == lfd) {
				int cfd = accept(lfd, 0, 0);

				if (cfd >= 0) {
					ev.events = EPOLLIN;
					ev.data.fd = cfd;
					epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
				}
			} else if (fd == mfd) {
				// Drain mouse packets already queued (kernel/mouse.c's
				// MOUSEBUFSIZE-deep ring) before redrawing once, instead of
				// a full-frame redraw per packet - VirtualBox's PS/2 mouse
				// can enqueue a burst of these for one physical motion, and
				// without this a single mouse wiggle was doing dozens of
				// full-screen redraws. Peek for more with a zero-timeout
				// epoll_wait() rather than looping read() unconditionally:
				// this kernel has no O_NONBLOCK (kernel/mouse.c's
				// mouseread() genuinely sleeps when the ring is empty), so
				// an unconditional next read() would block the whole
				// compositor on the *next real* mouse move once drained.
				// If the peek instead reports some other fd ready, that fd
				// stays ready (epoll here is level-triggered) and gets
				// picked up by the outer epoll_wait() on its next spin, so
				// nothing is lost by stopping the drain here.
				//
				// MOUSE_DRAIN_MAX bounds this regardless: a real mouse under
				// continuous motion can keep the ring nonempty indefinitely
				// (the guest reading packets no faster than the host/VM
				// keeps enqueuing them), and an unbounded loop here would
				// starve every *other* fd - accept() on lfd, wire_recv() on
				// a client socket - for as long as the flood lasts. That
				// previously manifested as exactly this: touch the desktop
				// icon, the mouse motion needed to reach and click it kept
				// this loop spinning, so the newly launched terminal's
				// CREATE_SURFACE never got processed and the screen never
				// got redrawn (cursor looked frozen) until the mouse went
				// idle.
				int drained = 0;
				int wi = -1, pressed = 0, sent_pressed = -1; /* sent_pressed:
					-1 means "nothing sent yet this burst" */
				// Damage tracking for this burst's flush_dirty() call
				// below: the common case (plain cursor motion, no
				// drag, no click that changes stacking/focus) only
				// needs the cursor's old+new bbox redrawn.
				// topology_changed covers anything that could affect
				// more than that (a raise-to-front reshuffles what's
				// visible under every other window, not just this
				// one) by falling back to a full-screen flush instead
				// of trying to track it precisely.
				int old_cursor_x = cursor_x, old_cursor_y = cursor_y;
				int drag_win = dragging, old_wx = 0, old_wy = 0;
				int topology_changed = 0;

				if (drag_win >= 0) {
					old_wx = windows[drag_win].x;
					old_wy = windows[drag_win].y;
				}

				for (; drained < MOUSE_DRAIN_MAX; drained++) {
					struct mousepkt pkt;
					int in_title;
					struct epoll_event peek;

					if (read(mfd, &pkt, sizeof(pkt)) != (int)sizeof(pkt))
						break;
					cursor_x += pkt.dx;
					cursor_y -= pkt.dy;
					if (cursor_x < 0) cursor_x = 0;
					if (cursor_x >= (int)fbsurf.w) cursor_x = (int)fbsurf.w - 1;
					if (cursor_y < 0) cursor_y = 0;
					if (cursor_y >= (int)fbsurf.h) cursor_y = (int)fbsurf.h - 1;

					pressed = pkt.buttons & 0x1;
					wi = window_at(cursor_x, cursor_y, &in_title);
					if (dragging >= 0) {
						windows[dragging].x = cursor_x - drag_off_x;
						windows[dragging].y = cursor_y - drag_off_y;
						if (!pressed)
							dragging = -1;
					} else if (pressed) {
						if (wi >= 0 && in_close_btn(&windows[wi], cursor_x, cursor_y)) {
							// Server-initiated close: just close the client's
							// socket. Its next gui_recv_event() then sees the
							// same "connection closed" it already handles for
							// a client-initiated disconnect (gui/terminal.c's
							// own read loop writes "exit\n" to bash and exits
							// cleanly on exactly that) - no new client-side
							// protocol message needed.
							remove_window(epfd, wi);
							wi = -1;
							topology_changed = 1;
						} else if (wi >= 0 && in_max_btn(&windows[wi], cursor_x, cursor_y)) {
							struct window *w = &windows[wi];
							int nw, nh;

							if (!w->maximized) {
								w->restore_x = w->x;
								w->restore_y = w->y;
								w->restore_content_w = (int)w->surf.w;
								w->restore_content_h = (int)w->surf.h;
								nw = (int)fbsurf.w - 2 * BORDER;
								nh = (int)fbsurf.h - TOPBAR_RESERVED_H - TITLEBAR_H - BORDER;
								if (resize_window_content(wi, nw, nh) == 0) {
									w->x = 0;
									w->y = TOPBAR_RESERVED_H;
									w->maximized = 1;
								}
							} else if (resize_window_content(wi, w->restore_content_w,
							                                   w->restore_content_h) == 0) {
								w->x = w->restore_x;
								w->y = w->restore_y;
								w->maximized = 0;
							}
							// Same reasoning as the close branch above: a click on
							// this window's own decoration chrome is not content-
							// area input, so it must not also be forwarded as a
							// pointer event below - GUI_MSG_RESIZE (just sent by
							// resize_window_content()) already puts this client's
							// incoming queue at its SOCKQLEN (3, include/socket.h)
							// depth limit on its own; stacking an unnecessary
							// pointer-event send on top of that reliably
							// overran it and wedged this wire_send() the same
							// way the mouse-burst coalescing comment below
							// already describes for plain pointer events.
							wi = -1;
							topology_changed = 1;
						} else if (wi >= 0 && in_min_btn(&windows[wi], cursor_x, cursor_y)) {
							windows[wi].minimized = 1;
							if (wi == focus_idx)
								set_focus(-1);
							send_task_list();
							wi = -1; // see the max-button branch's own comment above
							topology_changed = 1;
						} else if (wi >= 0) {
							zorder_raise(wi);
							topology_changed = 1;
							// NO_FOCUS windows (desktop bar/icon) never take
							// keyboard focus - see handle_create_surface()'s own
							// comment. in_title is already forced 0 for
							// BORDERLESS windows (window_at() above), so a
							// borderless-but-focusable window (the login
							// screen) still can't be dragged (no titlebar to
							// grab) without needing a separate check here.
							if (!(windows[wi].flags & GUI_WIN_NO_FOCUS))
								set_focus(wi);
							if (in_title) {
								dragging = wi;
								drag_off_x = cursor_x - windows[wi].x;
								drag_off_y = cursor_y - windows[wi].y;
								// Window hasn't moved yet this iteration -
								// its position right now is its "old" one
								// for the damage rect below.
								drag_win = wi;
								old_wx = windows[wi].x;
								old_wy = windows[wi].y;
							}
						}
					}

					// Send a pointer event immediately on every button-state
					// transition (a client's click detection is edge-
					// triggered on consecutive events - see gui/desktop.c's
					// icon_was_pressed - so a press-then-release that both
					// land inside one drained burst must still produce two
					// separate messages, not just the final state), but
					// otherwise skip sends for a burst's interior packets;
					// one send after the loop below covers the settled
					// position. A client's whole incoming queue is only
					// SOCKQLEN (3, include/socket.h) messages deep - sending
					// per-packet here (up to MOUSE_DRAIN_MAX) reliably
					// overran that and wedged the compositor's wire_send()
					// on a client (gui/desktop.c) that was itself briefly
					// not draining its socket (blocked in launch_terminal()'s
					// waitpid()), which looked like a permanent hang: this
					// keeps traffic to roughly the same one-ish message per
					// burst the pre-coalescing code sent per packet.
					if (wi >= 0 && pressed != sent_pressed) {
						struct window *w = &windows[wi];
						struct gui_msg_pointer_event pev;
						int cx_off = (w->flags & GUI_WIN_BORDERLESS) ? 0 : BORDER;
						int cy_off = (w->flags & GUI_WIN_BORDERLESS) ? 0 : TITLEBAR_H;

						pev.type = GUI_MSG_POINTER_EVENT;
						pev.x = cursor_x - (w->x + cx_off);
						pev.y = cursor_y - (w->y + cy_off);
						pev.buttons = pressed;
						try_send(w->fd, &pev, sizeof(pev));
						sent_pressed = pressed;
					}

					if (epoll_wait(epfd, &peek, 1, 0) != 1 || peek.data.fd != mfd)
						break;
				}

				// Final settled position/button-state, unconditionally (not
				// gated on a transition): a burst that ends mid-drag or
				// mid-hover with no further button change would otherwise
				// never tell the client where the pointer actually stopped.
				if (wi >= 0) {
					struct window *w = &windows[wi];
					struct gui_msg_pointer_event pev;
					int cx_off = (w->flags & GUI_WIN_BORDERLESS) ? 0 : BORDER;
					int cy_off = (w->flags & GUI_WIN_BORDERLESS) ? 0 : TITLEBAR_H;

					pev.type = GUI_MSG_POINTER_EVENT;
					pev.x = cursor_x - (w->x + cx_off);
					pev.y = cursor_y - (w->y + cy_off);
					pev.buttons = pressed;
					try_send(w->fd, &pev, sizeof(pev));
				}
				redraw_all();
				if (topology_changed) {
					mark_dirty_full();
				} else {
					int cw = cursor_img.pixels ? (int)cursor_img.w : CURSOR_W;
					int ch = cursor_img.pixels ? (int)cursor_img.h : CURSOR_H;

					mark_dirty(old_cursor_x, old_cursor_y, old_cursor_x + cw, old_cursor_y + ch);
					mark_dirty(cursor_x, cursor_y, cursor_x + cw, cursor_y + ch);
					if (drag_win >= 0) {
						struct window *dw = &windows[drag_win];

						mark_dirty(old_wx, old_wy, old_wx + decor_w(dw), old_wy + decor_h(dw));
						mark_dirty(dw->x, dw->y, dw->x + decor_w(dw), dw->y + decor_h(dw));
					}
				}
				flush_dirty();
			} else if (fd == 0) {
				unsigned char c;

				if (read(0, &c, 1) != 1)
					continue;
				if (focus_idx >= 0) {
					struct gui_msg_key_event kev;

					kev.type = GUI_MSG_KEY_EVENT;
					kev.ch = c;
					try_send(windows[focus_idx].fd, &kev, sizeof(kev));
				}
			} else {
				union gui_msg msg;
				int r = wire_recv(fd, &msg, sizeof(msg), 0);
				int wi;

				if (r <= 0) {
					for (wi = 0; wi < MAXWIN; wi++)
						if (windows[wi].inuse && windows[wi].fd == fd) {
							remove_window(epfd, wi);
							redraw_all();
							mark_dirty_full();
							flush_dirty();
							break;
						}
					continue;
				}

				switch (msg.type) {
				case GUI_MSG_CREATE_SURFACE:
					handle_create_surface(epfd, fd, &msg.create_surface);
					redraw_all();
					mark_dirty_full();
					flush_dirty();
					break;
				case GUI_MSG_COMMIT: {
					// The hot repaint path (e.g. a terminal echoing a
					// keystroke) - only this window's own decor rect
					// needs to reach the real framebuffer, not the
					// whole screen. A block-scoped loop index, not the
					// outer event-loop's `i`, which this switch nests
					// inside.
					int cwi;

					wi = -1;
					for (cwi = 0; cwi < MAXWIN; cwi++)
						if (windows[cwi].inuse && windows[cwi].fd == fd) {
							windows[cwi].committed = 1;
							wi = cwi;
							break;
						}
					redraw_all();
					if (wi >= 0) {
						struct window *w = &windows[wi];

						mark_dirty(w->x, w->y, w->x + decor_w(w), w->y + decor_h(w));
					} else {
						mark_dirty_full();
					}
					flush_dirty();
					load_wallpaper_deferred();
					break;
				}
				case GUI_MSG_DESTROY:
					for (wi = 0; wi < MAXWIN; wi++)
						if (windows[wi].inuse && windows[wi].fd == fd) {
							remove_window(epfd, wi);
							break;
						}
					redraw_all();
					mark_dirty_full();
					flush_dirty();
					break;
				case GUI_MSG_QUERY_SCREEN: {
					union gui_msg reply;

					memset(&reply, 0, sizeof(reply));
					reply.screen_info.type = GUI_MSG_SCREEN_INFO;
					reply.screen_info.screen_w = fbsurf.w;
					reply.screen_info.screen_h = fbsurf.h;
					wire_send(fd, &reply, sizeof(reply), -1);
					break;
				}
				case GUI_MSG_TASK_SUBSCRIBE:
					taskbar_fd = fd;
					send_task_list();
					break;
				case GUI_MSG_TASK_ACTION:
					for (wi = 0; wi < MAXWIN; wi++)
						if (windows[wi].inuse && windows[wi].surface_id == msg.task_action.surface_id)
							break;
					if (wi < MAXWIN) {
						// A click on the already-focused, already-visible
						// task minimizes it (the usual taskbar toggle);
						// anything else (minimized, or just not focused)
						// restores/raises/focuses it instead.
						if (wi == focus_idx && !windows[wi].minimized) {
							windows[wi].minimized = 1;
							set_focus(-1);
						} else {
							windows[wi].minimized = 0;
							zorder_raise(wi);
							if (!(windows[wi].flags & GUI_WIN_NO_FOCUS))
								set_focus(wi);
						}
						send_task_list();
						redraw_all();
						mark_dirty_full();
						flush_dirty();
					}
					break;
				default:
					break;
				}
			}
		}
	}
}
