/* desktop: GUI roadmap ToaruOS-style rewrite - the desktop shell
 * login_gui.c hands off to after a successful login (replacing the
 * old direct exec into a bare terminal). One process, two independent
 * compositor connections (the wire protocol has no notion of a client
 * owning more than one surface per connection - see gui_proto.h's own
 * comment - so two gui_connect()s, not one client with two surfaces,
 * is the simplest way for one process to own two windows):
 *
 *   - a full-width top bar (borderless, pinned at (0,0)) showing a
 *     live clock (kernel/lapic.c's cmostime(), wired up as the new
 *     SYS_date syscall purely for this)
 *   - a small desktop icon (borderless, fixed position) that launches
 *     /usr/bin/terminal (fork()+execve()) on click
 *
 * Both independently load the wallpaper raw asset and crop exactly
 * the region behind their own known screen position, the same "fake
 * transparency" trick login_gui.c uses for its glass box - the
 * compositor itself also draws the wallpaper as its native background
 * (gui/compositor.c), so this keeps both borderless windows visually
 * seamless against it with no real alpha compositing needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "gfx.h"
#include "gui_proto.h"
#include "libgui.h"
#include "ttf.h"

extern long syscall(long, ...);

#define SYS_date 69
#define SYS_uptime 14

// Double-click threshold, in timer ticks (include/defs.h's `ticks`,
// read via the uptime() syscall - kernel/lapic.c's own comment admits
// its LAPIC timer isn't precisely calibrated to a real 100Hz, so this
// is an approximate "about half a second", not an exact one).
#define DOUBLE_CLICK_TICKS 50

// Mirrors include/date.h's struct rtcdate field-for-field - user-space
// has no access to poc-os's own include/ tree (see gui/compositor.c's
// own struct fb_info for the established precedent of a local
// ABI-compatible copy instead).
struct rtcdate {
	unsigned int second, minute, hour, day, month, year;
};

#define PANEL_H 27
#define ICON_W 72
#define ICON_H 72
#define ICON_X 20
#define ICON_Y 40
#define ICON_IMG_SIZE 48

#define COLOR_BAR_TINT   0x101418
#define BAR_ALPHA        170
#define COLOR_BAR_TEXT   0xFFFFFF
#define COLOR_ACCENT_TOP 0x5DA3EC
#define COLOR_ACCENT_BOT 0x3889DC
#define COLOR_ICON_LABEL 0xFFFFFF

// Taskbar (ToaruOS-style minimize/restore, gui/compositor.c's own
// GUI_MSG_TASK_LIST/GUI_MSG_TASK_ACTION): one small button per open
// window, in the gap between the "poc-os" label and the clock.
#define TASK_BTN_X0  90
#define TASK_BTN_W   110
#define TASK_BTN_GAP 6
#define TASK_BTN_MAXX_MARGIN 150 /* leave this much room for the clock */

struct desktop {
	struct gui_conn bar, icon;
	struct gfx_surface wallpaper_bar, wallpaper_icon;
	struct gfx_image_rgba icon_img;
	struct ttf_font *sans, *sans_bold;
	struct gui_task_entry tasks[GUI_MAX_TASKS];
	int ntasks;
};

// Shared by render_bar() (drawing) and main()'s event loop (hit-
// testing) so the two can never disagree about where a task button
// actually is. Returns 0 (with *x0/*x1 filled in) if button i would
// fit before the clock's reserved margin, -1 if it wouldn't (render_bar()
// just stops drawing at that point; the click handler treats it as "no
// button here").
static int
task_btn_rect(int bar_w, int i, int *x0, int *x1)
{
	*x0 = TASK_BTN_X0 + i * (TASK_BTN_W + TASK_BTN_GAP);
	*x1 = *x0 + TASK_BTN_W;
	return *x1 > bar_w - TASK_BTN_MAXX_MARGIN ? -1 : 0;
}

// Double-fork: poc-os's wait() (kernel/proc.c) is a plain blocking
// xv6-style wait with no WNOHANG/waitpid, and desktop.c's event loop
// can't afford to block on it for however long a terminal session
// lasts. Forking twice and having the middle child exit immediately
// orphans the real terminal process straight to initproc (/usr/bin/
// init, bash/poc/dinit.c - see kernel/proc.c's exit()'s own "pass
// abandoned children to init" comment), which already reaps its
// children in a loop - so the terminal never becomes desktop.c's own
// zombie, and the one wait() call below only blocks for the instant
// it takes the middle child to fork-and-exit.
static void
launch_terminal(void)
{
	int pid = fork();

	if (pid == 0) {
		if (fork() == 0) {
			char *argv[] = { "terminal", 0 };
			char *envp[] = { 0 };

			execve("/usr/bin/terminal", argv, envp);
			_exit(1);
		}
		_exit(0);
	}
	if (pid > 0)
		waitpid(pid, 0, 0);
}

static void
format_clock(char *buf, unsigned long bufsz)
{
	struct rtcdate r;
	static const char *mname[] = {
		"", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};

	if (syscall(SYS_date, &r) < 0 || r.month == 0 || r.month > 12) {
		buf[0] = 0;
		return;
	}
	snprintf(buf, bufsz, "%02u:%02u:%02u  %s %u, %u",
	         r.hour, r.minute, r.second, mname[r.month], r.day, r.year);
}

static void
render_bar(struct desktop *d)
{
	char clock[64];
	int w = (int)d->bar.surface.w;

	if (d->wallpaper_bar.pixels)
		gfx_blit(&d->bar.surface, 0, 0, &d->wallpaper_bar, 0, 0, w, PANEL_H);
	else
		gfx_fill_rect(&d->bar.surface, 0, 0, w, PANEL_H, 0x181818);
	gfx_fill_rect_alpha(&d->bar.surface, 0, 0, w, PANEL_H, COLOR_BAR_TINT, BAR_ALPHA);
	gfx_fill_rect_gradient(&d->bar.surface, 0, PANEL_H - 2, w, PANEL_H, COLOR_ACCENT_TOP, COLOR_ACCENT_BOT);

	ttf_draw_string(&d->bar.surface, d->sans_bold, 12, 6, "poc-os", 13, COLOR_BAR_TEXT);

	{
		int i;

		for (i = 0; i < d->ntasks; i++) {
			int x0, x1;
			struct gui_task_entry *t = &d->tasks[i];
			unsigned int bg = t->focused ? COLOR_ACCENT_TOP : 0xFFFFFF;
			unsigned char alpha = t->focused ? 200 : (t->minimized ? 20 : 50);

			if (task_btn_rect(w, i, &x0, &x1) < 0)
				break;
			gfx_fill_rounded_rect_alpha(&d->bar.surface, x0, 3, x1, PANEL_H - 5, 6, bg, alpha);
			ttf_draw_string(&d->bar.surface, d->sans, x0 + 8, 6, t->title, 11, COLOR_BAR_TEXT);
		}
	}

	format_clock(clock, sizeof(clock));
	if (clock[0]) {
		int cw = ttf_string_width(d->sans, clock, 13);

		ttf_draw_string(&d->bar.surface, d->sans, w - cw - 14, 6, clock, 13, COLOR_BAR_TEXT);
	}

	gui_commit(&d->bar);
}

static void
render_icon(struct desktop *d, int hover)
{
	int img_x = (ICON_W - ICON_IMG_SIZE) / 2;
	const char *label = "Terminal";
	int label_w;

	if (d->wallpaper_icon.pixels)
		gfx_blit(&d->icon.surface, 0, 0, &d->wallpaper_icon, 0, 0, ICON_W, ICON_H);
	else
		gfx_fill_rect(&d->icon.surface, 0, 0, ICON_W, ICON_H, 0x181818);

	if (hover)
		gfx_fill_rounded_rect_alpha(&d->icon.surface, 2, 2, ICON_W - 2, ICON_H - 2, 10, 0xFFFFFF, 60);

	if (d->icon_img.pixels)
		gfx_blit_alpha(&d->icon.surface, img_x, 4, &d->icon_img, 0, 0, ICON_IMG_SIZE, ICON_IMG_SIZE);

	label_w = ttf_string_width(d->sans, label, 12);
	ttf_draw_string_shadow(&d->icon.surface, d->sans, (ICON_W - label_w) / 2, ICON_IMG_SIZE + 8,
	                        label, 12, COLOR_ICON_LABEL, 0x000000);

	gui_commit(&d->icon);
}

int
main(void)
{
	struct desktop d;
	struct epoll_event ev, events[2];
	int epfd;
	int icon_was_pressed = 0, icon_hover = 0, icon_hover_drawn = -1;
	int bar_was_pressed = 0;
	unsigned int last_click_tick = 0; /* 0 = no click yet this session */

	memset(&d, 0, sizeof(d));

	// Icon first, not the bar: it needs no screen_w (fixed ICON_X/
	// ICON_Y/ICON_W/ICON_H), so it can be created in one round trip -
	// and every GUI_MSG_SURFACE_CREATED reply already carries the real
	// screen_w/screen_h regardless of the requested surface's own size
	// (gui_proto.h's own comment on that struct), so the bar can just
	// read it off the icon's reply instead of the old probe/destroy/
	// reconnect/recreate dance this used to need to learn it. Net
	// effect: one fewer round trip overall, and the icon's own surface
	// - the thing the user actually clicks - exists and can be drawn
	// before the bar's does, instead of after.
	if (gui_connect(&d.icon, GUI_SOCK_PATH) < 0 ||
	    gui_create_surface(&d.icon, ICON_W, ICON_H, ICON_X, ICON_Y,
	                        GUI_WIN_BORDERLESS | GUI_WIN_NO_FOCUS | GUI_WIN_DESKTOP_BG, "desktop-icon") < 0) {
		printf("desktop: icon create failed\n");
		return 1;
	}

	if (gui_connect(&d.bar, GUI_SOCK_PATH) < 0 ||
	    gui_create_surface(&d.bar, (int)d.icon.screen_w, PANEL_H, 0, 0,
	                        GUI_WIN_BORDERLESS | GUI_WIN_NO_FOCUS, "desktop-bar") < 0) {
		printf("desktop: bar create failed\n");
		return 1;
	}
	gui_task_subscribe(&d.bar);

	// Commit both windows with their existing zero-asset fallbacks
	// (render_bar()/render_icon()'s own `pixels`-null checks - solid
	// colors, no text since d.sans/d.sans_bold are still null and
	// ttf_draw_string() is a no-op without a font) before loading
	// anything from disk. Same fix shape as gui/compositor.c's
	// load_wallpaper_deferred(): the bar and icon appear on screen
	// immediately after login instead of only once the wallpaper crop
	// and both font files have finished loading below.
	render_icon(&d, 0);
	render_bar(&d);

	// Fonts and the terminal icon image are both small, fast loads -
	// get the icon showing its real picture/label and the bar its real
	// text as soon as possible, well before the much larger wallpaper
	// crop below (a full-screen read) is anywhere near done.
	d.sans = ttf_load("/usr/share/fonts/dejavu/DejaVuSans.ttf");
	d.sans_bold = ttf_load("/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf");
	gfx_load_raw_rgba(&d.icon_img, "/usr/share/icons/terminal.raw");
	render_icon(&d, 0);
	render_bar(&d);

	// Load the full wallpaper exactly once (both crops below read from
	// this same copy) and deliberately never free it: kernel/sysproc.c's
	// sys_munmap() only allows unmapping from the exact current top of
	// the process's address space (see its own comment), but musl's
	// free() for a large allocation can call munmap() on it regardless
	// of what's been allocated since - freeing this ~2.3MB buffer after
	// the two small crop mallocs below violates that ordering and
	// crashed (found via trace prints bisecting exactly this call).
	// One-time process-lifetime leak instead - harmless for a single
	// long-lived desktop shell process.
	{
		struct gfx_surface full;

		gfx_load_raw(&full, "/usr/share/wallpaper.raw", &d.bar.surface);
		if (full.pixels) {
			d.wallpaper_bar.w = d.bar.surface.w;
			d.wallpaper_bar.h = PANEL_H;
			d.wallpaper_bar.pitch = d.bar.surface.pitch;
			d.wallpaper_bar.pixels = malloc((unsigned long)d.bar.surface.pitch * PANEL_H);
			if (d.wallpaper_bar.pixels)
				gfx_blit(&d.wallpaper_bar, 0, 0, &full, 0, 0, (int)full.w, PANEL_H);

			d.wallpaper_icon.w = ICON_W;
			d.wallpaper_icon.h = ICON_H;
			d.wallpaper_icon.pitch = d.icon.surface.pitch;
			d.wallpaper_icon.pixels = malloc((unsigned long)d.icon.surface.pitch * ICON_H);
			if (d.wallpaper_icon.pixels)
				gfx_blit(&d.wallpaper_icon, 0, 0, &full, ICON_X, ICON_Y, ICON_W, ICON_H);
		}
	}

	// Wallpaper crop is in now too - redraw both windows once more.
	render_icon(&d, 0);
	render_bar(&d);

	epfd = epoll_create1(0);
	if (epfd < 0) {
		printf("desktop: epoll_create1 failed\n");
		return 1;
	}
	ev.events = EPOLLIN;
	ev.data.fd = d.bar.fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, d.bar.fd, &ev);
	ev.data.fd = d.icon.fd;
	epoll_ctl(epfd, EPOLL_CTL_ADD, d.icon.fd, &ev);

	for (;;) {
		int n = epoll_wait(epfd, events, 2, 1000);
		int i;

		if (n < 0)
			continue;
		if (n == 0) {
			render_bar(&d);   /* clock tick */
			continue;
		}
		for (i = 0; i < n; i++) {
			struct gui_event gev;
			struct gui_conn *src = events[i].data.fd == d.bar.fd ? &d.bar : &d.icon;

			if (gui_recv_event(src, &gev) < 0) {
				printf("desktop: connection closed\n");
				return 1;
			}

			if (src == &d.bar && gev.type == GUI_EVENT_TASK_LIST) {
				d.ntasks = gev.tasklist.count;
				memcpy(d.tasks, gev.tasklist.tasks, sizeof(d.tasks));
				render_bar(&d);
				continue;
			}

			if (src == &d.bar && gev.type == GUI_EVENT_POINTER) {
				int pressed = (gev.pointer.buttons & 1) != 0;

				if (pressed && !bar_was_pressed) {
					int t;

					for (t = 0; t < d.ntasks; t++) {
						int x0, x1;

						if (task_btn_rect((int)d.bar.surface.w, t, &x0, &x1) < 0)
							break;
						if (gev.pointer.x >= x0 && gev.pointer.x < x1 &&
						    gev.pointer.y >= 0 && gev.pointer.y < PANEL_H) {
							gui_task_action(&d.bar, d.tasks[t].surface_id);
							break;
						}
					}
				}
				bar_was_pressed = pressed;
				continue;
			}

			if (gev.type != GUI_EVENT_POINTER || src != &d.icon)
				continue;

			icon_hover = gev.pointer.x >= 0 && gev.pointer.x < ICON_W &&
			             gev.pointer.y >= 0 && gev.pointer.y < ICON_H;
			if ((gev.pointer.buttons & 1) && !icon_was_pressed && icon_hover) {
				unsigned int now = (unsigned int)syscall(SYS_uptime);

				// Require a second press within DOUBLE_CLICK_TICKS of
				// the first, same as any real desktop's double-click -
				// a single click alone must not launch anything. The
				// first click of a pair is only ever recorded, never
				// acted on; last_click_tick resets to 0 once a launch
				// fires so a third rapid click doesn't immediately
				// re-launch as a "second" click of a new pair.
				if (last_click_tick && now - last_click_tick <= DOUBLE_CLICK_TICKS) {
					launch_terminal();
					last_click_tick = 0;
				} else {
					last_click_tick = now;
				}
			}
			icon_was_pressed = (gev.pointer.buttons & 1) != 0;
			// Only the hover flag changes what render_icon() actually
			// draws - redrawing (and gui_commit()'s synchronous round
			// trip to the compositor) on every single pointer event,
			// including the many fired while the mouse just sits still
			// or moves around inside an already-hovered icon, made the
			// compositor's wire_send() to this window's socket back up
			// (kernel/socket.c's 3-message queue cap) and block, which
			// stalls its single-threaded event loop entirely - looked
			// like the whole system freezing (mouse included) while
			// the cursor was anywhere near the icon.
			if (icon_hover != icon_hover_drawn) {
				render_icon(&d, icon_hover);
				icon_hover_drawn = icon_hover;
			}
		}
	}
}
