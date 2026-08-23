// libgui: the client half of the GUI roadmap phase 7-10 stack - a
// real shared object (build/libgui.so, DT_NEEDED-linked, not another
// copy of the phase-2-style single-interpreter libc.so pattern) used
// by every client of gui/compositor.c (Phase 8): gui/login_gui.c
// (Phase 9), gui/terminal.c (Phase 10), and future apps. Mirrors
// ToaruOS's own libtoaru_graphics/libtoaru_yutani split.
#ifndef LIBGUI_H
#define LIBGUI_H

#include "gfx.h"
#include "gui_proto.h" // struct gui_task_entry/GUI_MAX_TASKS, used below

struct gui_conn {
  int fd;
  int surface_id;
  int shmfd; // the surface's current backing shm fd - remembered so a
             // later GUI_MSG_RESIZE (gui_recv_event()) can close the
             // old one once the new mapping is in place
  struct gfx_surface surface;
  unsigned int screen_w, screen_h;  // whole framebuffer, not just this surface
};

// Connects to the compositor's well-known AF_UNIX socket
// (GUI_SOCK_PATH, gui_proto.h). Returns 0 on success, -1 on failure.
int gui_connect(struct gui_conn *c, const char *sockpath);

// Requests a w x h surface with the given title (may be NULL), placed
// at (x,y) - or auto-cascaded, the original behavior, if x and y are
// both -1 - with the given gui_proto.h GUI_WIN_* flags (0 for a normal
// decorated app window). On success, c->surface is mmap()'d and ready
// to draw into via gfx.h's functions; c->surface_id identifies it to
// the compositor for gui_commit()/gui_destroy(); c->screen_w/screen_h
// report the whole framebuffer's size. Returns 0 on success, -1 on
// failure.
int gui_create_surface(struct gui_conn *c, int w, int h, int x, int y,
                        int flags, const char *title);

// Asks the compositor for the framebuffer's current size, without
// allocating a window/shm block the way gui_create_surface() would -
// for a client (gui/login_gui.c) that needs screen_w/screen_h up
// front to compute a centered placement, before it has anything to
// actually display. Returns 0 on success, -1 on failure.
int gui_query_screen(struct gui_conn *c, unsigned int *screen_w, unsigned int *screen_h);

// Tells the compositor the surface's current pixel contents are ready
// to be composited (blitted into the real framebuffer on the next
// redraw). Returns 0 on success, -1 on failure.
int gui_commit(struct gui_conn *c);

// Tells the compositor to drop the surface and closes the connection.
void gui_destroy(struct gui_conn *c);

// Registers this connection to receive GUI_EVENT_TASK_LIST updates
// whenever the compositor's window set/focus/minimized state changes
// (gui/desktop.c's bar - the one place any client needs this: it's
// the only thing that draws a taskbar). Returns 0 on success, -1 on
// failure.
int gui_task_subscribe(struct gui_conn *c);

// Tells the compositor a taskbar button for this surface_id (from a
// received GUI_EVENT_TASK_LIST entry) was clicked - minimizes it if
// already focused and visible, otherwise restores/raises/focuses it.
// Returns 0 on success, -1 on failure.
int gui_task_action(struct gui_conn *c, int surface_id);

enum { GUI_EVENT_KEY, GUI_EVENT_POINTER, GUI_EVENT_FOCUS, GUI_EVENT_TASK_LIST, GUI_EVENT_RESIZE };

struct gui_event {
  int type;
  union {
    struct { int ch; } key;
    struct { int x, y, buttons; } pointer;
    struct { int focused; } focus;
    // GUI_EVENT_TASK_LIST: mirrors gui_msg_task_list's own tasks/count
    // (gui_proto.h) - copied through as-is, not reinterpreted.
    struct { int count; struct gui_task_entry tasks[GUI_MAX_TASKS]; } tasklist;
    // GUI_EVENT_RESIZE: c->surface is already updated (new mmap, old
    // one unmapped) by the time this is returned - the caller just
    // needs to know a resize happened (to reflow/re-render at the new
    // size), not the new dimensions themselves (already in
    // c->surface.w/h).
  };
};

// Blocks until the compositor delivers one input event (only ever
// sent while this surface has focus, for key events - pointer/focus
// events are sent regardless). Returns 0 on success, -1 on failure
// (including the compositor closing the connection).
int gui_recv_event(struct gui_conn *c, struct gui_event *ev);

#endif
