/* GStreamer Wayland Library
 *
 * Copyright (C) 2011 Intel Corporation
 * Copyright (C) 2011 Sreerenj Balachandran <sreerenj.balachandran@intel.com>
 * Copyright (C) 2014 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <linux/input.h>
#include <unistd.h>

#include "gstwlwindow.h"
#include "gstwlutils.h"
#include "gstimxcommon.h"

#include "fullscreen-shell-unstable-v1-client-protocol.h"
#include "single-pixel-buffer-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "alpha-compositing-unstable-v1-client-protocol.h"
#include "linux-explicit-synchronization-unstable-v1-client-protocol.h"

#define GST_CAT_DEFAULT gst_wl_window_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

typedef struct _GstWlWindowPrivate
{
  GObject parent_instance;

  GMutex *render_lock;

  GstWlDisplay *display;
  struct wl_surface *area_surface;
  struct wl_surface *area_surface_wrapper;
  struct wl_subsurface *area_subsurface;
  struct wp_viewport *area_viewport;
  struct wl_surface *video_surface;
  struct wl_surface *video_surface_wrapper;
  struct wl_subsurface *video_subsurface;
  struct wp_viewport *video_viewport;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *xdg_toplevel;
  struct zwp_linux_surface_synchronization_v1 *surface_sync;
  gboolean configured;
  GCond configure_cond;
  GMutex configure_mutex;

  gboolean redraw_pending;
  GCond redraw_wait;

  struct wl_shell_surface *shell_surface;
  struct zwp_blending_v1 *blend_func;

  /* the size and position of the area_(sub)surface */
  GstVideoRectangle render_rectangle;

  /* the size and position of the video_subsurface */
  GstVideoRectangle video_rectangle;

  /* the size of the video in the buffers */
  gint video_width, video_height;

  /* video width scaled according to par */
  gint scaled_width;

  enum wl_output_transform buffer_transform;

  /* when this is not set both the area_surface and the video_surface are not
   * visible and certain steps should be skipped */
  gboolean is_area_surface_mapped;

  GMutex window_lock;
  GstWlBuffer *next_buffer;
  GstVideoInfo *next_video_info;
  GstWlBuffer *staged_buffer;
  gboolean clear_window;
  struct wl_callback *frame_callback;
  struct wl_callback *commit_callback;

  GMutex commit_lock;

  /* the coordinate of video crop */
  gint src_x, src_y, src_width, src_height;

  /* video buffer scale */
  guint scale;

  /* mouse location when click */
  gint pointer_x, pointer_y;
  /* fullscreen window size */
  gint fullscreen_width, fullscreen_height;
} GstWlWindowPrivate;

G_DEFINE_TYPE_WITH_CODE (GstWlWindow, gst_wl_window, G_TYPE_OBJECT,
    G_ADD_PRIVATE (GstWlWindow)
    GST_DEBUG_CATEGORY_INIT (gst_wl_window_debug,
        "wlwindow", 0, "wlwindow library");
    );

/* resize trigger margin in pixel */
#define RESIZE_MARGIN 20

enum
{
  CLOSED,
  MAP,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void gst_wl_window_finalize (GObject * gobject);

static void gst_wl_window_update_borders (GstWlWindow * self);

static void gst_wl_window_commit_buffer (GstWlWindow * self,
    GstWlBuffer * buffer);

static void
pointer_handle_enter (void *data, struct wl_pointer *pointer,
    uint32_t serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
  GstWlWindow *self = data;
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);

  priv->pointer_x = wl_fixed_to_int (sx);
  priv->pointer_y = wl_fixed_to_int (sy);
}

static void
pointer_handle_leave (void *data, struct wl_pointer *pointer,
    uint32_t serial, struct wl_surface *surface)
{
}

static void
pointer_handle_motion (void *data, struct wl_pointer *pointer,
    uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
}

static void
pointer_handle_button (void *data, struct wl_pointer *wl_pointer,
    uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
  GstWlWindow *self = data;
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);

  if (!priv->xdg_toplevel)
    return;

  if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
    struct wl_seat *seat = gst_wl_display_get_seat (priv->display);
    if (priv->render_rectangle.w - priv->pointer_x <= RESIZE_MARGIN
        && priv->render_rectangle.h - priv->pointer_y <= RESIZE_MARGIN)
      xdg_toplevel_resize (priv->xdg_toplevel, seat, serial,
          XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
    else
      xdg_toplevel_move (priv->xdg_toplevel, seat, serial);
  }
}

static void
pointer_handle_axis (void *data, struct wl_pointer *wl_pointer,
    uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static const struct wl_pointer_listener pointer_listener = {
  pointer_handle_enter,
  pointer_handle_leave,
  pointer_handle_motion,
  pointer_handle_button,
  pointer_handle_axis,
};

static void
touch_handle_down (void *data, struct wl_touch *wl_touch,
    uint32_t serial, uint32_t time, struct wl_surface *surface,
    int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
  GstWlWindow *self = data;
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);
  struct wl_seat *seat;

  if (!priv->xdg_toplevel)
    return;

  seat = gst_wl_display_get_seat (priv->display);
  xdg_toplevel_move (priv->xdg_toplevel, seat, serial);
}

static void
touch_handle_up (void *data, struct wl_touch *wl_touch,
    uint32_t serial, uint32_t time, int32_t id)
{
}

static void
touch_handle_motion (void *data, struct wl_touch *wl_touch,
    uint32_t time, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
}

static void
touch_handle_frame (void *data, struct wl_touch *wl_touch)
{
}

static void
touch_handle_cancel (void *data, struct wl_touch *wl_touch)
{
}

static const struct wl_touch_listener touch_listener = {
  touch_handle_down,
  touch_handle_up,
  touch_handle_motion,
  touch_handle_frame,
  touch_handle_cancel,
};

static void
handle_xdg_toplevel_close (void *data, struct xdg_toplevel *xdg_toplevel)
{
  GstWlWindow *self = data;

  GST_DEBUG ("XDG toplevel got a \"close\" event.");
  g_signal_emit (self, signals[CLOSED], 0);
}

static void
handle_xdg_toplevel_configure (void *data, struct xdg_toplevel *xdg_toplevel,
    int32_t width, int32_t height, struct wl_array *states)
{
  GstWlWindow *self = data;
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);
  const uint32_t *state;

  GST_DEBUG ("XDG toplevel got a \"configure\" event, [ %d, %d ].",
      width, height);

  wl_array_for_each (state, states) {
    switch (*state) {
      case XDG_TOPLEVEL_STATE_FULLSCREEN:
      case XDG_TOPLEVEL_STATE_MAXIMIZED:
      case XDG_TOPLEVEL_STATE_RESIZING:
      case XDG_TOPLEVEL_STATE_ACTIVATED:
        break;
    }
  }

  if (width <= 2 * RESIZE_MARGIN || height <= 2 * RESIZE_MARGIN)
    return;

  g_mutex_lock (priv->render_lock);
  gst_wl_window_set_render_rectangle (self, 0, 0, width, height);
  g_mutex_unlock (priv->render_lock);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
  handle_xdg_toplevel_configure,
  handle_xdg_toplevel_close,
};

static void
handle_xdg_surface_configure (void *data, struct xdg_surface *xdg_surface,
    uint32_t serial)
{
  GstWlWindow *self = data;
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);

  xdg_surface_ack_configure (xdg_surface, serial);

  g_mutex_lock (&priv->configure_mutex);
  priv->configured = TRUE;
  g_cond_signal (&priv->configure_cond);
  g_mutex_unlock (&priv->configure_mutex);
}

static const struct xdg_surface_listener xdg_surface_listener = {
  handle_xdg_surface_configure,
};

static gboolean
gst_wl_poll_wait_fence (int32_t fence)
{
  GstPollFD pollfd = GST_POLL_FD_INIT;
  GstPoll *fence_poll = gst_poll_new (TRUE);

  pollfd.fd = fence;
  gst_poll_add_fd (fence_poll, &pollfd);
  gst_poll_fd_ctl_read (fence_poll, &pollfd, TRUE);
  gst_poll_fd_ctl_write (fence_poll, &pollfd, TRUE);

  if (gst_poll_wait (fence_poll, GST_CLOCK_TIME_NONE) < 0) {
    GST_ERROR ("wait on fence failed, errno %d", errno);
    gst_poll_free (fence_poll);
    return FALSE;
  }

  GST_DEBUG ("wait on fence %d done", fence);
  gst_poll_free (fence_poll);

  return TRUE;
}

static void
buffer_fenced_release (void *data,
    struct zwp_linux_buffer_release_v1 *release, int32_t fence)
{
  GstWlBuffer *buffer = data;
  GstBuffer *current_gstbuffer = gst_wl_buffer_get_current_gstbuffer (buffer);

  g_assert (release == gst_wl_buffer_get_buffer_release (buffer));

  gst_wl_buffer_set_used_by_compositor (buffer, FALSE);
  zwp_linux_buffer_release_v1_destroy (gst_wl_buffer_get_buffer_release (buffer));
  gst_wl_buffer_set_buffer_release (buffer, NULL);
  GST_LOG ("wl_buffer::fenced_release %d (GstBuffer: %p)",
      fence, current_gstbuffer);

  if (fence > 0) {
    gst_wl_poll_wait_fence (fence);
    close (fence);
    if (buffer)
      gst_buffer_unref (current_gstbuffer);
  }

}

static void
buffer_immediate_release (void *data,
    struct zwp_linux_buffer_release_v1 *release)
{
  GstWlBuffer *buffer = data;
  GstBuffer *current_gstbuffer = gst_wl_buffer_get_current_gstbuffer (buffer);

  g_assert (release == gst_wl_buffer_get_buffer_release (buffer));

  gst_wl_buffer_set_used_by_compositor (buffer, FALSE);
  zwp_linux_buffer_release_v1_destroy (gst_wl_buffer_get_buffer_release (buffer));
  gst_wl_buffer_set_buffer_release (buffer, NULL);
  GST_LOG ("wl_buffer::immediate_release (GstBuffer: %p)",
      current_gstbuffer);

  /* unref should be last, because it may end up destroying the GstWlBuffer */
  gst_buffer_unref (current_gstbuffer);
}

static const struct zwp_linux_buffer_release_v1_listener buffer_release_listener
    = {
  buffer_fenced_release,
  buffer_immediate_release,
};


static void
gst_wl_window_class_init (GstWlWindowClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = gst_wl_window_finalize;

  signals[CLOSED] = g_signal_new ("closed", G_TYPE_FROM_CLASS (gobject_class),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[MAP] = g_signal_new ("map", G_TYPE_FROM_CLASS (gobject_class),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

}

static void
gst_wl_window_init (GstWlWindow * self)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);

  priv->configured = TRUE;
  priv->redraw_pending = FALSE;
  g_cond_init (&priv->configure_cond);
  g_cond_init (&priv->redraw_wait);
  g_mutex_init (&priv->configure_mutex);
  g_mutex_init (&priv->window_lock);
  g_mutex_init (&priv->commit_lock);

  priv->src_x = 0;
  priv->src_y = 0;
  priv->src_width = -1;
  priv->src_height = 0;
  priv->scale = 1;
  priv->fullscreen_width = -1;
  priv->fullscreen_height = -1;
}

static void
gst_wl_window_finalize (GObject * gobject)
{
  GstWlWindow *self = GST_WL_WINDOW (gobject);
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);

  gst_wl_display_callback_destroy (priv->display, &priv->frame_callback);
  gst_wl_display_callback_destroy (priv->display, &priv->commit_callback);

  g_mutex_lock (&priv->window_lock);
  priv->redraw_pending = FALSE;
  g_cond_signal (&priv->redraw_wait);
  g_mutex_unlock (&priv->window_lock);

  g_cond_clear (&priv->configure_cond);
  g_cond_clear (&priv->redraw_wait);
  g_mutex_clear (&priv->configure_mutex);
  g_mutex_clear (&priv->window_lock);
  g_mutex_clear (&priv->commit_lock);

  if (priv->staged_buffer)
    gst_wl_buffer_unref_buffer (priv->staged_buffer);

  if (priv->xdg_toplevel)
    xdg_toplevel_destroy (priv->xdg_toplevel);
  if (priv->xdg_surface)
    xdg_surface_destroy (priv->xdg_surface);

  if (priv->video_viewport)
    wp_viewport_destroy (priv->video_viewport);

  if (priv->surface_sync)
    zwp_linux_surface_synchronization_v1_destroy (priv->surface_sync);

  wl_proxy_wrapper_destroy (priv->video_surface_wrapper);

  if (priv->blend_func)
    zwp_blending_v1_destroy (priv->blend_func);

  wl_subsurface_destroy (priv->video_subsurface);
  wl_surface_destroy (priv->video_surface);

  if (priv->area_subsurface)
    wl_subsurface_destroy (priv->area_subsurface);

  if (priv->area_viewport)
    wp_viewport_destroy (priv->area_viewport);

  wl_proxy_wrapper_destroy (priv->area_surface_wrapper);
  wl_surface_destroy (priv->area_surface);

  g_clear_object (&priv->display);

  G_OBJECT_CLASS (gst_wl_window_parent_class)->finalize (gobject);
}

static GstWlWindow *
gst_wl_window_new_internal (GstWlDisplay * display, GMutex * render_lock)
{
  GstWlWindow *self;
  GstWlWindowPrivate *priv;
  struct wl_compositor *compositor;
  struct wl_event_queue *event_queue;
  struct wl_region *region;
  struct wp_viewporter *viewporter;
  struct zwp_alpha_compositing_v1 *alpha_compositing;
  struct zwp_linux_explicit_synchronization_v1 *explicit_sync;
  gint width, height;

  self = g_object_new (GST_TYPE_WL_WINDOW, NULL);
  priv = gst_wl_window_get_instance_private (self);
  priv->display = g_object_ref (display);
  priv->render_lock = render_lock;
  g_cond_init (&priv->configure_cond);

  compositor = gst_wl_display_get_compositor (display);
  priv->area_surface = wl_compositor_create_surface (compositor);
  priv->video_surface = wl_compositor_create_surface (compositor);

  priv->area_surface_wrapper = wl_proxy_create_wrapper (priv->area_surface);
  priv->video_surface_wrapper = wl_proxy_create_wrapper (priv->video_surface);

  event_queue = gst_wl_display_get_event_queue (display);
  wl_proxy_set_queue ((struct wl_proxy *) priv->area_surface_wrapper,
      event_queue);
  wl_proxy_set_queue ((struct wl_proxy *) priv->video_surface_wrapper,
      event_queue);

  /* embed video_surface in area_surface */
  priv->video_subsurface =
      wl_subcompositor_get_subsurface (gst_wl_display_get_subcompositor
      (display), priv->video_surface, priv->area_surface);
  wl_subsurface_set_desync (priv->video_subsurface);

  viewporter = gst_wl_display_get_viewporter (display);
  if (viewporter) {
    priv->area_viewport = wp_viewporter_get_viewport (viewporter,
        priv->area_surface);
    priv->video_viewport = wp_viewporter_get_viewport (viewporter,
        priv->video_surface);
  }

  alpha_compositing = gst_wl_display_get_alpha_compositing (display);
  if (alpha_compositing)
    priv->blend_func =
        zwp_alpha_compositing_v1_get_blending (alpha_compositing,
        priv->area_surface);

  explicit_sync = gst_wl_display_get_explicit_sync (display);
  if (explicit_sync)
    priv->surface_sync =
        zwp_linux_explicit_synchronization_v1_get_synchronization(explicit_sync,
        priv->video_surface_wrapper);

  /* never accept input events on the video surface */
  region = wl_compositor_create_region (compositor);
  wl_surface_set_input_region (priv->video_surface, region);
  wl_region_destroy (region);

  width = gst_wl_display_get_width (display);
  height = gst_wl_display_get_height (display);

  if (!gst_wl_init_surface_state (display, self)) {
    priv->fullscreen_width = width;
    priv->fullscreen_height = height - PANEL_HEIGH;
    priv->scale = 1;
    GST_WARNING
        ("init surface_state fail, fallback to scale=%d fullscreen (%dx%d)",
        priv->scale, priv->fullscreen_width, priv->fullscreen_height);
  }

  return self;
}

void
gst_wl_window_ensure_fullscreen (GstWlWindow * self, gboolean fullscreen)
{
  GstWlWindowPrivate *priv;

  g_return_if_fail (self);

  priv = gst_wl_window_get_instance_private (self);
  if (fullscreen)
    xdg_toplevel_set_fullscreen (priv->xdg_toplevel, NULL);
  else
    xdg_toplevel_unset_fullscreen (priv->xdg_toplevel);
}

GstWlWindow *
gst_wl_window_new_toplevel (GstWlDisplay * display, const GstVideoInfo * info,
    gboolean fullscreen, GMutex * render_lock)
{
  GstWlWindow *self;
  GstWlWindowPrivate *priv;
  struct xdg_wm_base *xdg_wm_base;
  struct zwp_fullscreen_shell_v1 *fullscreen_shell;

  self = gst_wl_window_new_internal (display, render_lock);
  priv = gst_wl_window_get_instance_private (self);

  xdg_wm_base = gst_wl_display_get_xdg_wm_base (display);
  fullscreen_shell = gst_wl_display_get_fullscreen_shell_v1 (display);

  /* Check which protocol we will use (in order of preference) */
  if (xdg_wm_base) {
    gint64 timeout;
    struct wl_pointer *pointer;
    struct wl_touch *touch;

    /* First create the XDG surface */
    priv->xdg_surface = xdg_wm_base_get_xdg_surface (xdg_wm_base,
        priv->area_surface);
    if (!priv->xdg_surface) {
      GST_ERROR ("Unable to get xdg_surface");
      goto error;
    }
    xdg_surface_add_listener (priv->xdg_surface, &xdg_surface_listener, self);

    /* Then the toplevel */
    priv->xdg_toplevel = xdg_surface_get_toplevel (priv->xdg_surface);
    if (!priv->xdg_toplevel) {
      GST_ERROR ("Unable to get xdg_toplevel");
      goto error;
    }
    xdg_toplevel_add_listener (priv->xdg_toplevel,
        &xdg_toplevel_listener, self);
    if (g_get_prgname ()) {
      xdg_toplevel_set_app_id (priv->xdg_toplevel, g_get_prgname ());
    } else {
      xdg_toplevel_set_app_id (priv->xdg_toplevel, "org.gstreamer.wayland");
    }

    pointer = gst_wl_display_get_pointer (display);
    touch = gst_wl_display_get_touch (display);
    if (pointer)
      wl_pointer_add_listener (pointer, &pointer_listener, self);

    if (touch) {
      wl_touch_set_user_data (touch, self);
      wl_touch_add_listener (touch, &touch_listener, self);
    }

    gst_wl_window_ensure_fullscreen (self, fullscreen);

    /* Finally, commit the xdg_surface state as toplevel */
    priv->configured = FALSE;
    wl_surface_commit (priv->area_surface);
    wl_display_flush (gst_wl_display_get_display (display));

    g_mutex_lock (&priv->configure_mutex);
    timeout = g_get_monotonic_time () + 100 * G_TIME_SPAN_MILLISECOND;
    while (!priv->configured) {
      if (!g_cond_wait_until (&priv->configure_cond, &priv->configure_mutex,
              timeout)) {
        GST_WARNING ("The compositor did not send configure event.");
        break;
      }
    }
    g_mutex_unlock (&priv->configure_mutex);
  } else if (fullscreen_shell) {
    zwp_fullscreen_shell_v1_present_surface (fullscreen_shell,
        priv->area_surface, ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_ZOOM, NULL);
  } else {
    GST_ERROR ("Unable to use either xdg_wm_base or zwp_fullscreen_shell.");
    goto error;
  }

  /* render_rectangle is already set via toplevel_configure in
   * xdg_shell fullscreen mode */
  if (!(xdg_wm_base && fullscreen)) {
    gint width, height;
    gint preferred_width = gst_wl_display_get_preferred_width (display);
    gint preferred_height = gst_wl_display_get_preferred_height (display);
    if (preferred_width > 0 && preferred_height > 0) {
      width = preferred_width;
      height = preferred_height;
    } else if (priv->fullscreen_width <= 0) {
      /* set the initial size to be the same as the reported video size */
      width =
          gst_util_uint64_scale_int_round (info->width, info->par_n,
          info->par_d);
      height = info->height;
    } else {
      width = priv->fullscreen_width;
      height = priv->fullscreen_height;
    }

    gst_wl_window_set_render_rectangle (self, 0, 0, width, height);
  }

  return self;

error:
  g_object_unref (self);
  return NULL;
}

GstWlWindow *
gst_wl_window_new_in_surface (GstWlDisplay * display,
    struct wl_surface *parent, GMutex * render_lock)
{
  GstWlWindow *self;
  GstWlWindowPrivate *priv;
  struct wl_region *region;

  self = gst_wl_window_new_internal (display, render_lock);
  priv = gst_wl_window_get_instance_private (self);

  /* do not accept input events on the area surface when embedded */
  region =
      wl_compositor_create_region (gst_wl_display_get_compositor (display));
  wl_surface_set_input_region (priv->area_surface, region);
  wl_region_destroy (region);

  /* embed in parent */
  priv->area_subsurface =
      wl_subcompositor_get_subsurface (gst_wl_display_get_subcompositor
      (display), priv->area_surface, parent);
  wl_subsurface_set_desync (priv->area_subsurface);

  wl_surface_commit (parent);

  return self;
}

GstWlDisplay *
gst_wl_window_get_display (GstWlWindow * self)
{
  GstWlWindowPrivate *priv;

  g_return_val_if_fail (self != NULL, NULL);

  priv = gst_wl_window_get_instance_private (self);
  return g_object_ref (priv->display);
}

struct wl_surface *
gst_wl_window_get_wl_surface (GstWlWindow * self)
{
  GstWlWindowPrivate *priv;

  g_return_val_if_fail (self != NULL, NULL);

  priv = gst_wl_window_get_instance_private (self);
  return priv->video_surface_wrapper;
}

struct wl_subsurface *
gst_wl_window_get_subsurface (GstWlWindow * self)
{
  GstWlWindowPrivate *priv;

  g_return_val_if_fail (self != NULL, NULL);

  priv = gst_wl_window_get_instance_private (self);
  return priv->area_subsurface;
}

struct wl_surface *
gst_wl_window_get_area_surface (GstWlWindow * self)
{
  GstWlWindowPrivate *priv;

  g_return_val_if_fail (self != NULL, NULL);

  priv = gst_wl_window_get_instance_private (self);
  return priv->area_surface;
}

gint
gst_wl_window_get_rectangle_w (GstWlWindow * self)
{
  GstWlWindowPrivate *priv;

  g_return_val_if_fail (self != NULL, -1);

  priv = gst_wl_window_get_instance_private (self);
  return priv->render_rectangle.w;
}

gint
gst_wl_window_get_rectangle_h (GstWlWindow * self)
{
  GstWlWindowPrivate *priv;

  g_return_val_if_fail (self != NULL, -1);

  priv = gst_wl_window_get_instance_private (self);
  return priv->render_rectangle.h;
}

gboolean
gst_wl_window_is_toplevel (GstWlWindow * self)
{
  GstWlWindowPrivate *priv;

  g_return_val_if_fail (self != NULL, FALSE);

  priv = gst_wl_window_get_instance_private (self);
  return (priv->xdg_toplevel != NULL);
}

static void
gst_wl_window_resize_video_surface (GstWlWindow * self, gboolean commit)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);
  GstVideoRectangle src = { 0, };
  GstVideoRectangle dst = { 0, };
  GstVideoRectangle res;

  wl_fixed_t src_x = wl_fixed_from_int (priv->src_x / priv->scale);
  wl_fixed_t src_y = wl_fixed_from_int (priv->src_y / priv->scale);
  wl_fixed_t src_width = wl_fixed_from_int (-1 / priv->scale);
  wl_fixed_t src_height = wl_fixed_from_int (-1 / priv->scale);

  switch (priv->buffer_transform) {
    case WL_OUTPUT_TRANSFORM_NORMAL:
    case WL_OUTPUT_TRANSFORM_180:
    case WL_OUTPUT_TRANSFORM_FLIPPED:
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
      src_width = wl_fixed_from_int (priv->src_width / priv->scale);
      src_height = wl_fixed_from_int (priv->src_height / priv->scale);
      src.w = priv->scaled_width;
      src.h = priv->video_height;
      break;
    case WL_OUTPUT_TRANSFORM_90:
    case WL_OUTPUT_TRANSFORM_270:
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
      src_width = wl_fixed_from_int (priv->src_height / priv->scale);
      src_height = wl_fixed_from_int (priv->src_width / priv->scale);
      src.w = priv->video_height;
      src.h = priv->scaled_width;
      break;
  }

  dst.w = priv->render_rectangle.w;
  dst.h = priv->render_rectangle.h;

  /* center the video_subsurface inside area_subsurface */
  if (priv->video_viewport) {
    gst_video_center_rect (&src, &dst, &res, TRUE);
    wp_viewport_set_destination (priv->video_viewport, res.w, res.h);
    if (src_width != wl_fixed_from_int (-1 / priv->scale)
        && src_height != wl_fixed_from_int (-1 / priv->scale))
      wp_viewport_set_source (priv->video_viewport,
          src_x, src_y, src_width, src_height);
  } else {
    gst_video_center_rect (&src, &dst, &res, FALSE);
  }

  wl_subsurface_set_position (priv->video_subsurface, res.x, res.y);
  wl_surface_set_buffer_transform (priv->video_surface_wrapper,
      priv->buffer_transform);

  if (commit)
    wl_surface_commit (priv->video_surface_wrapper);

  priv->video_rectangle = res;
}

static void
gst_wl_window_set_opaque (GstWlWindow * self, const GstVideoInfo * info)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);
  struct wl_compositor *compositor;
  struct wl_region *region;

  /* Set area opaque */
  compositor = gst_wl_display_get_compositor (priv->display);

  if (!GST_VIDEO_INFO_HAS_ALPHA (info)) {
    /* for platform support overlay, video should not overlap graphic
       FIXME. Not sure whether still need this change */
    if (HAS_DCSS ())
      return;

    /* Set video opaque */
    region = wl_compositor_create_region (compositor);
    wl_region_add (region, 0, 0, G_MAXINT32, G_MAXINT32);
    wl_surface_set_opaque_region (priv->video_surface, region);
    wl_region_destroy (region);
  }
}

static void
frame_redraw_callback (void *data, struct wl_callback *callback, uint32_t time)
{
  GstWlWindow *self = data;
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);
  GstWlBuffer *next_buffer;

  GST_INFO ("frame_redraw_cb ");

  wl_callback_destroy (callback);
  priv->frame_callback = NULL;

  g_mutex_lock (&priv->window_lock);
  next_buffer = priv->next_buffer = priv->staged_buffer;
  priv->staged_buffer = NULL;
  priv->redraw_pending = FALSE;
  g_cond_signal (&priv->redraw_wait);
  g_mutex_unlock (&priv->window_lock);

  if (next_buffer || priv->clear_window)
    gst_wl_window_commit_buffer (self, next_buffer);

  if (next_buffer)
    gst_wl_buffer_unref_buffer (next_buffer);
}

static const struct wl_callback_listener frame_callback_listener = {
  frame_redraw_callback
};

static void
gst_wl_window_commit_buffer (GstWlWindow * self, GstWlBuffer * buffer)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);
  GstVideoInfo *info = priv->next_video_info;
  struct wl_callback *callback;
  GstBuffer *current_gstbuffer;
  struct zwp_linux_buffer_release_v1 *buffer_release;
  gboolean used_by_compositor;

  if (G_UNLIKELY (info)) {
    priv->scaled_width =
        gst_util_uint64_scale_int_round (info->width, info->par_n, info->par_d);
    priv->video_width = info->width;
    priv->video_height = info->height;

    wl_subsurface_set_sync (priv->video_subsurface);
    gst_wl_window_resize_video_surface (self, FALSE);
    gst_wl_window_set_opaque (self, info);
  }

  g_mutex_lock (&priv->commit_lock);
  if (G_LIKELY (buffer)) {
    current_gstbuffer = gst_wl_buffer_get_current_gstbuffer (buffer);
    used_by_compositor = gst_wl_buffer_get_used_by_compositor (buffer);
    if (!used_by_compositor && priv->surface_sync) {
      GST_DEBUG ("use explicit sync create buffer release (GstBuffer: %p)",
        current_gstbuffer);
      gst_wl_buffer_set_buffer_release (buffer,
          zwp_linux_surface_synchronization_v1_get_release (priv->surface_sync));
      buffer_release = gst_wl_buffer_get_buffer_release (buffer);
      zwp_linux_buffer_release_v1_add_listener (buffer_release,
          &buffer_release_listener, buffer);
    }

    callback = wl_surface_frame (priv->video_surface_wrapper);
    priv->frame_callback = callback;
    wl_callback_add_listener (callback, &frame_callback_listener, self);
    gst_wl_buffer_attach (buffer, priv->video_surface_wrapper);
    wl_surface_set_buffer_scale (priv->video_surface_wrapper, priv->scale);
    wl_surface_damage_buffer (priv->video_surface_wrapper, 0, 0, G_MAXINT32,
        G_MAXINT32);
    wl_surface_commit (priv->video_surface_wrapper);

    if (!priv->is_area_surface_mapped) {
      gst_wl_window_update_borders (self);
      wl_surface_commit (priv->area_surface_wrapper);
      priv->is_area_surface_mapped = TRUE;
      g_signal_emit (self, signals[MAP], 0);
    }
  } else {
    /* clear both video and parent surfaces */
    wl_surface_attach (priv->video_surface_wrapper, NULL, 0, 0);
    wl_surface_set_buffer_scale (priv->video_surface_wrapper, priv->scale);
    wl_surface_commit (priv->video_surface_wrapper);
    wl_surface_attach (priv->area_surface_wrapper, NULL, 0, 0);
    wl_surface_commit (priv->area_surface_wrapper);
    priv->is_area_surface_mapped = FALSE;
    priv->clear_window = FALSE;
  }

  if (G_UNLIKELY (info)) {
    /* commit also the parent (area_surface) in order to change
     * the position of the video_subsurface */
    wl_surface_commit (priv->area_surface_wrapper);
    wl_subsurface_set_desync (priv->video_subsurface);
    gst_video_info_free (priv->next_video_info);
    priv->next_video_info = NULL;
  }

  g_mutex_unlock (&priv->commit_lock);
}

static void
commit_callback (void *data, struct wl_callback *callback, uint32_t serial)
{
  GstWlWindow *self = data;
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);
  GstWlBuffer *next_buffer;

  wl_callback_destroy (callback);
  priv->commit_callback = NULL;

  g_mutex_lock (&priv->window_lock);
  next_buffer = priv->next_buffer;
  g_mutex_unlock (&priv->window_lock);

  gst_wl_window_commit_buffer (self, next_buffer);

  if (next_buffer)
    gst_wl_buffer_unref_buffer (next_buffer);
}

static const struct wl_callback_listener commit_listener = {
  commit_callback
};

gboolean
gst_wl_window_render (GstWlWindow * self, GstWlBuffer * buffer,
    const GstVideoInfo * info)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);
  gboolean ret = TRUE;

  if (G_LIKELY (buffer))
    gst_wl_buffer_ref_gst_buffer (buffer);

  g_mutex_lock (&priv->window_lock);
  if (G_UNLIKELY (info))
    priv->next_video_info = gst_video_info_copy (info);

  while (priv->redraw_pending)
    g_cond_wait (&priv->redraw_wait, &priv->window_lock);

  if (priv->next_buffer && priv->staged_buffer) {
    GST_LOG_OBJECT (self, "buffer %p dropped (replaced)", priv->staged_buffer);
    gst_wl_buffer_unref_buffer (priv->staged_buffer);
    ret = FALSE;
  }

  if (!priv->next_buffer) {
    priv->next_buffer = buffer;
    priv->redraw_pending = TRUE;
    priv->commit_callback =
        gst_wl_display_sync (priv->display, &commit_listener, self);
    wl_display_flush (gst_wl_display_get_display (priv->display));
  } else {
    priv->staged_buffer = buffer;
  }
  if (!buffer)
    priv->clear_window = TRUE;

  g_mutex_unlock (&priv->window_lock);

  return ret;
}

/* Update the buffer used to draw black borders. When we have viewporter
 * support, this is a scaled up 1x1 image, and without we need an black image
 * the size of the rendering areay. */
static void
gst_wl_window_update_borders (GstWlWindow * self)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);
  gint width, height;
  GstBuffer *buf;
  struct wl_buffer *wlbuf;
  struct wp_single_pixel_buffer_manager_v1 *single_pixel;
  GstWlBuffer *gwlbuf;

  if (gst_wl_display_get_viewporter (priv->display)) {
    wp_viewport_set_destination (priv->area_viewport,
        priv->render_rectangle.w, priv->render_rectangle.h);

    if (priv->is_area_surface_mapped) {
      /* The area_surface is already visible and only needed to get resized.
       * We don't need to attach a new buffer and are done here. */
      return;
    }
  }

  if (gst_wl_display_get_viewporter (priv->display)) {
    width = height = 1;
  } else {
    width = priv->render_rectangle.w;
    height = priv->render_rectangle.h;
  }

  /* draw the area_subsurface */
  single_pixel =
      gst_wl_display_get_single_pixel_buffer_manager_v1 (priv->display);
  if (width == 1 && height == 1 && single_pixel) {
    buf = gst_buffer_new_allocate (NULL, 1, NULL);
    wlbuf =
        wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer (single_pixel,
        0, 0, 0, 0xffffffffU);
  } else {
    GstVideoFormat format;
    GstVideoInfo info;
    GstAllocator *alloc;

    /* we want WL_SHM_FORMAT_XRGB8888 */
    format = GST_VIDEO_FORMAT_BGRx;
    gst_video_info_set_format (&info, format, width, height);
    alloc = gst_shm_allocator_get ();

    buf = gst_buffer_new_allocate (alloc, info.size, NULL);
    gst_buffer_memset (buf, 0, 0, info.size);

    wlbuf =
        gst_wl_shm_memory_construct_wl_buffer (gst_buffer_peek_memory (buf, 0),
        priv->display, &info);

    g_object_unref (alloc);
  }

  gwlbuf = gst_buffer_add_wl_buffer (buf, wlbuf, priv->display);
  gst_wl_buffer_attach (gwlbuf, priv->area_surface_wrapper);
  wl_surface_damage_buffer (priv->area_surface_wrapper, 0, 0, G_MAXINT32,
      G_MAXINT32);

  /* at this point, the GstWlBuffer keeps the buffer
   * alive and will free it on wl_buffer::release */
  gst_buffer_unref (buf);
}

static void
gst_wl_window_update_geometry (GstWlWindow * self)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);

  /* position the area inside the parent - needs a parent commit to apply */
  if (priv->area_subsurface) {
    wl_subsurface_set_position (priv->area_subsurface, priv->render_rectangle.x,
        priv->render_rectangle.y);
  }

  if (priv->is_area_surface_mapped)
    gst_wl_window_update_borders (self);

  if (!priv->configured)
    return;

  if (priv->scaled_width != 0) {
    g_mutex_lock (&priv->commit_lock);
    wl_subsurface_set_sync (priv->video_subsurface);
    gst_wl_window_resize_video_surface (self, TRUE);
  }

  wl_surface_commit (priv->area_surface_wrapper);

  if (priv->scaled_width != 0) {
    wl_subsurface_set_desync (priv->video_subsurface);
    g_mutex_unlock (&priv->commit_lock);
  }
}

void
gst_wl_window_set_render_rectangle (GstWlWindow * self, gint x, gint y,
    gint w, gint h)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);

  if (priv->render_rectangle.x == x && priv->render_rectangle.y == y &&
      priv->render_rectangle.w == w && priv->render_rectangle.h == h)
    return;

  priv->render_rectangle.x = x;
  priv->render_rectangle.y = y;
  priv->render_rectangle.w = w;
  priv->render_rectangle.h = h;

  gst_wl_window_update_geometry (self);
}

void
gst_wl_window_set_source_crop (GstWlWindow * self, GstBuffer * buffer)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);
  GstVideoCropMeta *crop = NULL;
  crop = gst_buffer_get_video_crop_meta (buffer);

  if (crop) {
    GST_DEBUG ("buffer crop x=%d y=%d width=%d height=%d\n",
        crop->x, crop->y, crop->width, crop->height);
    priv->src_x = crop->x;
    priv->src_y = crop->y;
    priv->src_width = crop->width;
    priv->src_height = crop->height;
  } else {
    priv->src_width = -1;
  }
}

void
gst_wl_window_set_alpha (GstWlWindow * self, gfloat alpha)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);

  if (priv && priv->blend_func) {
    zwp_blending_v1_set_alpha (priv->blend_func,
        wl_fixed_from_double (alpha));
    if (alpha < 1.0)
      zwp_blending_v1_set_blending (priv->blend_func,
          ZWP_BLENDING_V1_BLENDING_EQUATION_FROMSOURCE);
    else
      zwp_blending_v1_set_blending (priv->blend_func,
          ZWP_BLENDING_V1_BLENDING_EQUATION_PREMULTIPLIED);
  }
}

const GstVideoRectangle *
gst_wl_window_get_render_rectangle (GstWlWindow * self)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);

  return &priv->render_rectangle;
}

static enum wl_output_transform
output_transform_from_orientation_method (GstVideoOrientationMethod method)
{
  switch (method) {
    case GST_VIDEO_ORIENTATION_IDENTITY:
      return WL_OUTPUT_TRANSFORM_NORMAL;
    case GST_VIDEO_ORIENTATION_90R:
      return WL_OUTPUT_TRANSFORM_90;
    case GST_VIDEO_ORIENTATION_180:
      return WL_OUTPUT_TRANSFORM_180;
    case GST_VIDEO_ORIENTATION_90L:
      return WL_OUTPUT_TRANSFORM_270;
    case GST_VIDEO_ORIENTATION_HORIZ:
      return WL_OUTPUT_TRANSFORM_FLIPPED;
    case GST_VIDEO_ORIENTATION_VERT:
      return WL_OUTPUT_TRANSFORM_FLIPPED_180;
    case GST_VIDEO_ORIENTATION_UL_LR:
      return WL_OUTPUT_TRANSFORM_FLIPPED_90;
    case GST_VIDEO_ORIENTATION_UR_LL:
      return WL_OUTPUT_TRANSFORM_FLIPPED_270;
    default:
      g_assert_not_reached ();
  }
}

void
gst_wl_window_set_rotate_method (GstWlWindow * self,
    GstVideoOrientationMethod method)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);

  priv->buffer_transform = output_transform_from_orientation_method (method);

  gst_wl_window_update_geometry (self);
}

void
gst_wl_window_set_scale (GstWlWindow * self, gint scale)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);

  priv->scale = scale;

  gst_wl_window_update_geometry (self);
}

guint
gst_wl_window_get_scale (GstWlWindow * self)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);

  return priv->scale;
}

void
gst_wl_window_set_fullscreen_width (GstWlWindow * self, gint fullscreen_width)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);

  priv->fullscreen_width = fullscreen_width;

  gst_wl_window_update_geometry (self);
}

void
gst_wl_window_set_fullscreen_height (GstWlWindow * self, gint fullscreen_height)
{
  GstWlWindowPrivate *priv = gst_wl_window_get_instance_private (self);

  priv->fullscreen_height = fullscreen_height;

  gst_wl_window_update_geometry (self);
}
