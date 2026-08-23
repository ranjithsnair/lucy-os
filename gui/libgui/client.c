#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "libgui.h"
#include "gui_proto.h"
#include "wire.h"

int
gui_connect(struct gui_conn *c, const char *sockpath)
{
  struct sockaddr_un addr;

  memset(c, 0, sizeof(*c));
  c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (c->fd < 0)
    return -1;

  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, sockpath);
  if (connect(c->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(c->fd);
    return -1;
  }
  return 0;
}

int
gui_create_surface(struct gui_conn *c, int w, int h, int x, int y,
                    int flags, const char *title)
{
  struct gui_msg_create_surface req;
  union gui_msg resp;
  int shmfd = -1;

  memset(&req, 0, sizeof(req));
  req.type = GUI_MSG_CREATE_SURFACE;
  req.w = w;
  req.h = h;
  req.x = x;
  req.y = y;
  req.flags = flags;
  if (title) {
    unsigned long n = strlen(title);

    if (n >= sizeof(req.title))
      n = sizeof(req.title) - 1;
    memcpy(req.title, title, n);
  }

  if (wire_send(c->fd, &req, sizeof(req), -1) != (int)sizeof(req))
    return -1;

  if (wire_recv(c->fd, &resp, sizeof(resp), &shmfd) <= 0)
    return -1;
  if (resp.type != GUI_MSG_SURFACE_CREATED || shmfd < 0)
    return -1;

  c->surface_id = resp.surface_created.surface_id;
  c->surface.w = resp.surface_created.w;
  c->surface.h = resp.surface_created.h;
  c->surface.pitch = resp.surface_created.pitch;
  c->surface.red_mask_size = resp.surface_created.red_mask_size;
  c->surface.red_field_pos = resp.surface_created.red_field_pos;
  c->surface.green_mask_size = resp.surface_created.green_mask_size;
  c->surface.green_field_pos = resp.surface_created.green_field_pos;
  c->surface.blue_mask_size = resp.surface_created.blue_mask_size;
  c->surface.blue_field_pos = resp.surface_created.blue_field_pos;
  c->screen_w = resp.surface_created.screen_w;
  c->screen_h = resp.surface_created.screen_h;

  c->shmfd = shmfd;
  c->surface.pixels = mmap(0, (unsigned long)resp.surface_created.pitch * resp.surface_created.h,
                            PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
  // Deliberately not close(shmfd): kernel/shm.c's refcounting only
  // tracks open struct file references, not active mmap() mappings -
  // closing here would drop this side's only reference the moment
  // both this client and the compositor have mmap()'d once each,
  // freeing the pages back to the kernel's kalloc() pool while still
  // actively mapped and written to (a real bug found and root-caused
  // via GDB during GUI roadmap phase 10 - the freed pages' contents
  // got corrupted by this surface's own pixel writes, then handed out
  // again elsewhere, corrupting the kernel's free-page list itself).
  // Kept open for the surface's lifetime instead; it's one fd per
  // surface for a client's whole process lifetime, not a real leak.
  return c->surface.pixels == MAP_FAILED ? -1 : 0;
}

int
gui_query_screen(struct gui_conn *c, unsigned int *screen_w, unsigned int *screen_h)
{
  struct gui_msg_query_screen req;
  union gui_msg resp;

  req.type = GUI_MSG_QUERY_SCREEN;
  if (wire_send(c->fd, &req, sizeof(req), -1) != (int)sizeof(req))
    return -1;

  if (wire_recv(c->fd, &resp, sizeof(resp), 0) <= 0)
    return -1;
  if (resp.type != GUI_MSG_SCREEN_INFO)
    return -1;

  *screen_w = resp.screen_info.screen_w;
  *screen_h = resp.screen_info.screen_h;
  return 0;
}

int
gui_commit(struct gui_conn *c)
{
  struct gui_msg_commit msg;

  msg.type = GUI_MSG_COMMIT;
  msg.surface_id = c->surface_id;
  return wire_send(c->fd, &msg, sizeof(msg), -1) == (int)sizeof(msg) ? 0 : -1;
}

void
gui_destroy(struct gui_conn *c)
{
  struct gui_msg_destroy msg;

  msg.type = GUI_MSG_DESTROY;
  msg.surface_id = c->surface_id;
  wire_send(c->fd, &msg, sizeof(msg), -1);
  close(c->fd);
}

int
gui_task_subscribe(struct gui_conn *c)
{
  struct gui_msg_task_subscribe msg;

  msg.type = GUI_MSG_TASK_SUBSCRIBE;
  return wire_send(c->fd, &msg, sizeof(msg), -1) == (int)sizeof(msg) ? 0 : -1;
}

int
gui_task_action(struct gui_conn *c, int surface_id)
{
  struct gui_msg_task_action msg;

  msg.type = GUI_MSG_TASK_ACTION;
  msg.surface_id = surface_id;
  return wire_send(c->fd, &msg, sizeof(msg), -1) == (int)sizeof(msg) ? 0 : -1;
}

int
gui_recv_event(struct gui_conn *c, struct gui_event *ev)
{
  union gui_msg raw;
  int recv_fd = -1;

  if (wire_recv(c->fd, &raw, sizeof(raw), &recv_fd) <= 0)
    return -1;

  switch (raw.type) {
  case GUI_MSG_KEY_EVENT:
    ev->type = GUI_EVENT_KEY;
    ev->key.ch = raw.key_event.ch;
    break;
  case GUI_MSG_POINTER_EVENT:
    ev->type = GUI_EVENT_POINTER;
    ev->pointer.x = raw.pointer_event.x;
    ev->pointer.y = raw.pointer_event.y;
    ev->pointer.buttons = raw.pointer_event.buttons;
    break;
  case GUI_MSG_FOCUS_EVENT:
    ev->type = GUI_EVENT_FOCUS;
    ev->focus.focused = raw.focus_event.focused;
    break;
  case GUI_MSG_TASK_LIST:
    ev->type = GUI_EVENT_TASK_LIST;
    ev->tasklist.count = raw.task_list.count;
    memcpy(ev->tasklist.tasks, raw.task_list.tasks, sizeof(ev->tasklist.tasks));
    break;
  case GUI_MSG_RESIZE: {
    // Same shm-fd-via-SCM_RIGHTS handoff gui_create_surface() already
    // does, just replacing an existing mapping instead of establishing
    // the first one - munmap the old surface and close its shmfd only
    // *after* the new one is confirmed mapped, so a failure here
    // leaves the caller's still-valid old surface alone.
    unsigned long newsize = (unsigned long)raw.resize.pitch * raw.resize.h;
    void *newpix;

    if (recv_fd < 0)
      return -1;
    newpix = mmap(0, newsize, PROT_READ | PROT_WRITE, MAP_SHARED, recv_fd, 0);
    if (newpix == MAP_FAILED) {
      close(recv_fd);
      return -1;
    }
    munmap(c->surface.pixels, (unsigned long)c->surface.pitch * c->surface.h);
    close(c->shmfd);
    c->shmfd = recv_fd;
    c->surface.pixels = newpix;
    c->surface.w = raw.resize.w;
    c->surface.h = raw.resize.h;
    c->surface.pitch = raw.resize.pitch;
    ev->type = GUI_EVENT_RESIZE;
    break;
  }
  default:
    return -1;
  }
  return 0;
}
