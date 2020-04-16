/* GStreamer Wayland video sink
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

#include <unistd.h>
#include <linux/input.h>

#include "wlwindow.h"
#include "wlshmallocator.h"
#include "wlbuffer.h"
#include "wlutils.h"

#include "gstimxcommon.h"

GST_DEBUG_CATEGORY_EXTERN (gstwayland_debug);
#define GST_CAT_DEFAULT gstwayland_debug

/* resize trigger margin in pixel */
#define RESIZE_MARGIN 20

enum
{
  CLOSED,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (GstWlWindow, gst_wl_window, G_TYPE_OBJECT);

static void gst_wl_window_finalize (GObject * gobject);

static void gst_wl_window_update_borders (GstWlWindow * window);

static void
pointer_handle_enter (void *data, struct wl_pointer *pointer,
    uint32_t serial, struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
  GstWlWindow *window = data;

  window->pointer_x = wl_fixed_to_int (sx);
  window->pointer_y = wl_fixed_to_int (sy);
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
  GstWlWindow *window = data;

  if (!window->xdg_toplevel)
    return;

  if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
    if (window->render_rectangle.w - window->pointer_x <= RESIZE_MARGIN
        && window->render_rectangle.h - window->pointer_y <= RESIZE_MARGIN)
      xdg_toplevel_resize (window->xdg_toplevel, window->display->seat, serial,
          XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
    else
      xdg_toplevel_move (window->xdg_toplevel, window->display->seat, serial);
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
  GstWlWindow *window = data;

  if (!window->xdg_toplevel)
    return;

  xdg_toplevel_move (window->xdg_toplevel, window->display->seat, serial);
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
  GstWlWindow *window = data;

  GST_DEBUG ("XDG toplevel got a \"close\" event.");
  g_signal_emit (window, signals[CLOSED], 0);
}

static void
handle_xdg_toplevel_configure (void *data, struct xdg_toplevel *xdg_toplevel,
    int32_t width, int32_t height, struct wl_array *states)
{
  GstWlWindow *window = data;
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

  g_mutex_lock (window->render_lock);
  gst_wl_window_set_render_rectangle (window, 0, 0, width, height);
  g_mutex_unlock (window->render_lock);
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
  handle_xdg_toplevel_configure,
  handle_xdg_toplevel_close,
};

static void
handle_xdg_surface_configure (void *data, struct xdg_surface *xdg_surface,
    uint32_t serial)
{
  GstWlWindow *window = data;
  xdg_surface_ack_configure (xdg_surface, serial);

  g_mutex_lock (&window->configure_mutex);
  window->configured = TRUE;
  g_cond_signal (&window->configure_cond);
  g_mutex_unlock (&window->configure_mutex);
}

static const struct xdg_surface_listener xdg_surface_listener = {
  handle_xdg_surface_configure,
};

static void
handle_ping (void *data, struct wl_shell_surface *wl_shell_surface,
    uint32_t serial)
{
  wl_shell_surface_pong (wl_shell_surface, serial);
}

static void
handle_configure (void *data, struct wl_shell_surface *wl_shell_surface,
    uint32_t edges, int32_t width, int32_t height)
{
  GstWlWindow *window = data;

  GST_DEBUG ("Windows configure: edges %x, width = %i, height %i", edges,
      width, height);

  if (width == 0 || height == 0)
    return;

  gst_wl_window_set_render_rectangle (window, 0, 0, width, height);
}

static void
handle_popup_done (void *data, struct wl_shell_surface *wl_shell_surface)
{
  GST_DEBUG ("Window popup done.");
}

static const struct wl_shell_surface_listener wl_shell_surface_listener = {
  handle_ping,
  handle_configure,
  handle_popup_done
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

  g_assert (release == buffer->buffer_release);

  buffer->used_by_compositor = FALSE;
  zwp_linux_buffer_release_v1_destroy (buffer->buffer_release);
  buffer->buffer_release = NULL;
  GST_LOG ("wl_buffer::fenced_release %d (GstBuffer: %p)",
      fence, buffer->current_gstbuffer);

  if (fence > 0) {
    gst_wl_poll_wait_fence (fence);
    close (fence);
    if (buffer)
      gst_buffer_unref (buffer->current_gstbuffer);
  }

}

static void
buffer_immediate_release (void *data,
    struct zwp_linux_buffer_release_v1 *release)
{
  GstWlBuffer *buffer = data;

  g_assert (release == buffer->buffer_release);

  buffer->used_by_compositor = FALSE;
  zwp_linux_buffer_release_v1_destroy (buffer->buffer_release);
  buffer->buffer_release = NULL;
  GST_LOG ("wl_buffer::immediate_release (GstBuffer: %p)",
      buffer->current_gstbuffer);

  /* unref should be last, because it may end up destroying the GstWlBuffer */
  gst_buffer_unref (buffer->current_gstbuffer);
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
}

static void
gst_wl_window_init (GstWlWindow * self)
{
  self->configured = TRUE;
  g_cond_init (&self->configure_cond);
  g_mutex_init (&self->configure_mutex);
  self->src_x = 0;
  self->src_y = 0;
  self->src_width = -1;
  self->src_height = 0;
  self->scale = 1;
  self->fullscreen_width = -1;
  self->fullscreen_height = -1;
}

static void
gst_wl_window_finalize (GObject * gobject)
{
  GstWlWindow *self = GST_WL_WINDOW (gobject);

  if (self->wl_shell_surface)
    wl_shell_surface_destroy (self->wl_shell_surface);

  if (self->xdg_toplevel)
    xdg_toplevel_destroy (self->xdg_toplevel);
  if (self->xdg_surface)
    xdg_surface_destroy (self->xdg_surface);

  if (self->video_viewport)
    wp_viewport_destroy (self->video_viewport);

  if (self->surface_sync)
    zwp_linux_surface_synchronization_v1_destroy (self->surface_sync);

  wl_proxy_wrapper_destroy (self->video_surface_wrapper);

  if (self->blend_func)
    zwp_blending_v1_destroy (self->blend_func);

  wl_subsurface_destroy (self->video_subsurface);
  wl_surface_destroy (self->video_surface);

  if (self->area_subsurface)
    wl_subsurface_destroy (self->area_subsurface);

  if (self->area_viewport)
    wp_viewport_destroy (self->area_viewport);

  wl_proxy_wrapper_destroy (self->area_surface_wrapper);
  wl_surface_destroy (self->area_surface);

  g_clear_object (&self->display);

  G_OBJECT_CLASS (gst_wl_window_parent_class)->finalize (gobject);
}

static GstWlWindow *
gst_wl_window_new_internal (GstWlDisplay * display, GMutex * render_lock)
{
  GstWlWindow *window;
  struct wl_region *region;

  window = g_object_new (GST_TYPE_WL_WINDOW, NULL);
  window->display = g_object_ref (display);
  window->render_lock = render_lock;
  g_cond_init (&window->configure_cond);

  window->area_surface = wl_compositor_create_surface (display->compositor);
  window->video_surface = wl_compositor_create_surface (display->compositor);

  window->area_surface_wrapper = wl_proxy_create_wrapper (window->area_surface);
  window->video_surface_wrapper =
      wl_proxy_create_wrapper (window->video_surface);

  wl_proxy_set_queue ((struct wl_proxy *) window->area_surface_wrapper,
      display->queue);
  wl_proxy_set_queue ((struct wl_proxy *) window->video_surface_wrapper,
      display->queue);

  /* embed video_surface in area_surface */
  window->video_subsurface =
      wl_subcompositor_get_subsurface (display->subcompositor,
      window->video_surface, window->area_surface);
  wl_subsurface_set_desync (window->video_subsurface);

  if (display->viewporter) {
    window->area_viewport = wp_viewporter_get_viewport (display->viewporter,
        window->area_surface);
    window->video_viewport = wp_viewporter_get_viewport (display->viewporter,
        window->video_surface);
  }

  if (display->alpha_compositing)
    window->blend_func =
        zwp_alpha_compositing_v1_get_blending (display->alpha_compositing,
        window->area_surface);

  if (display->explicit_sync)
    window->surface_sync =
        zwp_linux_explicit_synchronization_v1_get_synchronization
        (display->explicit_sync, window->video_surface_wrapper);

  /* never accept input events on the video surface */
  region = wl_compositor_create_region (display->compositor);
  wl_surface_set_input_region (window->video_surface, region);
  wl_region_destroy (region);

  if (!gst_wl_init_surface_state (display, window)) {
    window->fullscreen_width = display->width;
    window->fullscreen_height = display->height - PANEL_HEIGH;
    window->scale = 1;
    GST_WARNING
        ("init surface_state fail, fallback to scale=%d fullscreen (%dx%d)",
        window->scale, window->fullscreen_width, window->fullscreen_height);
  }

  return window;
}

void
gst_wl_window_ensure_fullscreen (GstWlWindow * window, gboolean fullscreen)
{
  if (!window)
    return;

  if (window->display->xdg_wm_base) {
    if (fullscreen)
      xdg_toplevel_set_fullscreen (window->xdg_toplevel, NULL);
    else
      xdg_toplevel_unset_fullscreen (window->xdg_toplevel);
  } else {
    if (fullscreen)
      wl_shell_surface_set_fullscreen (window->wl_shell_surface,
          WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE, 0, NULL);
    else
      wl_shell_surface_set_toplevel (window->wl_shell_surface);
  }
}

GstWlWindow *
gst_wl_window_new_toplevel (GstWlDisplay * display, const GstVideoInfo * info,
    gboolean fullscreen, GMutex * render_lock)
{
  GstWlWindow *window;

  window = gst_wl_window_new_internal (display, render_lock);

  /* Check which protocol we will use (in order of preference) */
  if (display->xdg_wm_base) {
    gint64 timeout;

    /* First create the XDG surface */
    window->xdg_surface = xdg_wm_base_get_xdg_surface (display->xdg_wm_base,
        window->area_surface);
    if (!window->xdg_surface) {
      GST_ERROR ("Unable to get xdg_surface");
      goto error;
    }
    xdg_surface_add_listener (window->xdg_surface, &xdg_surface_listener,
        window);

    /* Then the toplevel */
    window->xdg_toplevel = xdg_surface_get_toplevel (window->xdg_surface);
    if (!window->xdg_toplevel) {
      GST_ERROR ("Unable to get xdg_toplevel");
      goto error;
    }
    xdg_toplevel_add_listener (window->xdg_toplevel,
        &xdg_toplevel_listener, window);

    if (display->pointer)
      wl_pointer_add_listener (display->pointer, &pointer_listener, window);

    if (display->touch) {
      wl_touch_set_user_data (display->touch, window);
      wl_touch_add_listener (display->touch, &touch_listener, window);
    }

    gst_wl_window_ensure_fullscreen (window, fullscreen);

    /* Finally, commit the xdg_surface state as toplevel */
    window->configured = FALSE;
    wl_surface_commit (window->area_surface);
    wl_display_flush (display->display);

    g_mutex_lock (&window->configure_mutex);
    timeout = g_get_monotonic_time () + 100 * G_TIME_SPAN_MILLISECOND;
    while (!window->configured) {
      if (!g_cond_wait_until (&window->configure_cond, &window->configure_mutex,
              timeout)) {
        GST_WARNING ("The compositor did not send configure event.");
        break;
      }
    }
    g_mutex_unlock (&window->configure_mutex);
  } else if (display->wl_shell) {
    /* go toplevel */
    window->wl_shell_surface = wl_shell_get_shell_surface (display->wl_shell,
        window->area_surface);
    if (!window->wl_shell_surface) {
      GST_ERROR ("Unable to get wl_shell_surface");
      goto error;
    }

    wl_shell_surface_add_listener (window->wl_shell_surface,
        &wl_shell_surface_listener, window);
    gst_wl_window_ensure_fullscreen (window, fullscreen);
  } else if (display->fullscreen_shell) {
    zwp_fullscreen_shell_v1_present_surface (display->fullscreen_shell,
        window->area_surface, ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_ZOOM,
        NULL);
  } else {
    GST_ERROR ("Unable to use either wl_shell, xdg_wm_base or "
        "zwp_fullscreen_shell.");
    goto error;
  }

  /* render_rectangle is already set via toplevel_configure in
   * xdg_shell fullscreen mode */
  if (!(display->xdg_wm_base && fullscreen)) {
    gint width, height;

    if (display->preferred_width > 0 && display->preferred_height > 0) {
      width = display->preferred_width;
      height = display->preferred_height;
    } else if (window->fullscreen_width <= 0) {
      /* set the initial size to be the same as the reported video size */
      width =
          gst_util_uint64_scale_int_round (info->width, info->par_n,
          info->par_d);
      height = info->height;
    } else {
      width = window->fullscreen_width;
      height = window->fullscreen_height;
    }

    gst_wl_window_set_render_rectangle (window, 0, 0, width, height);
  }

  return window;

error:
  g_object_unref (window);
  return NULL;
}

GstWlWindow *
gst_wl_window_new_in_surface (GstWlDisplay * display,
    struct wl_surface * parent, GMutex * render_lock)
{
  GstWlWindow *window;
  struct wl_region *region;
  window = gst_wl_window_new_internal (display, render_lock);

  /* do not accept input events on the area surface when embedded */
  region = wl_compositor_create_region (display->compositor);
  wl_surface_set_input_region (window->area_surface, region);
  wl_region_destroy (region);

  /* embed in parent */
  window->area_subsurface =
      wl_subcompositor_get_subsurface (display->subcompositor,
      window->area_surface, parent);
  wl_subsurface_set_desync (window->area_subsurface);

  wl_surface_commit (parent);

  return window;
}

GstWlDisplay *
gst_wl_window_get_display (GstWlWindow * window)
{
  g_return_val_if_fail (window != NULL, NULL);

  return g_object_ref (window->display);
}

struct wl_surface *
gst_wl_window_get_wl_surface (GstWlWindow * window)
{
  g_return_val_if_fail (window != NULL, NULL);

  return window->video_surface_wrapper;
}

gboolean
gst_wl_window_is_toplevel (GstWlWindow * window)
{
  g_return_val_if_fail (window != NULL, FALSE);

  if (window->display->xdg_wm_base)
    return (window->xdg_toplevel != NULL);
  else
    return (window->wl_shell_surface != NULL);
}

static void
gst_wl_window_resize_video_surface (GstWlWindow * window, gboolean commit)
{
  GstVideoRectangle src = { 0, };
  GstVideoRectangle dst = { 0, };
  GstVideoRectangle res;

  wl_fixed_t src_x = wl_fixed_from_int (window->src_x / window->scale);
  wl_fixed_t src_y = wl_fixed_from_int (window->src_y / window->scale);
  wl_fixed_t src_width = wl_fixed_from_int (window->src_width / window->scale);
  wl_fixed_t src_height =
      wl_fixed_from_int (window->src_height / window->scale);

  /* center the video_subsurface inside area_subsurface */
  src.w = window->video_width;
  src.h = window->video_height;
  dst.w = window->render_rectangle.w;
  dst.h = window->render_rectangle.h;

  if (window->video_viewport) {
    gst_video_sink_center_rect (src, dst, &res, TRUE);
    wp_viewport_set_destination (window->video_viewport, res.w, res.h);
    if (src_width != wl_fixed_from_int (-1 / window->scale))
      wp_viewport_set_source (window->video_viewport,
          src_x, src_y, src_width, src_height);
  } else {
    gst_video_sink_center_rect (src, dst, &res, FALSE);
  }

  wl_subsurface_set_position (window->video_subsurface, res.x, res.y);

  if (commit)
    wl_surface_commit (window->video_surface_wrapper);

  window->video_rectangle = res;
}

static void
gst_wl_window_set_opaque (GstWlWindow * window, const GstVideoInfo * info)
{
  struct wl_region *region;

  if (!GST_VIDEO_INFO_HAS_ALPHA (info)) {
    /* for platform support overlay, video should not overlap graphic */
    if (HAS_DCSS () || HAS_DPU ())
      return;

    /* Set video opaque */
    region = wl_compositor_create_region (window->display->compositor);
    wl_region_add (region, 0, 0, G_MAXINT32, G_MAXINT32);
    wl_surface_set_opaque_region (window->video_surface, region);
    wl_region_destroy (region);
  }
}

void
gst_wl_window_render (GstWlWindow * window, GstWlBuffer * buffer,
    const GstVideoInfo * info)
{
  if (G_UNLIKELY (info)) {
    window->video_width =
        gst_util_uint64_scale_int_round (info->width, info->par_n, info->par_d);
    window->video_height = info->height;

    wl_subsurface_set_sync (window->video_subsurface);
    gst_wl_window_resize_video_surface (window, FALSE);
    gst_wl_window_set_opaque (window, info);
  }

  if (buffer && !buffer->used_by_compositor && window->surface_sync) {
    GST_DEBUG ("use explicit sync create buffer release (GstBuffer: %p)",
        buffer->current_gstbuffer);
    buffer->buffer_release =
        zwp_linux_surface_synchronization_v1_get_release (window->surface_sync);
    zwp_linux_buffer_release_v1_add_listener (buffer->buffer_release,
        &buffer_release_listener, buffer);
  }

  if (G_LIKELY (buffer)) {
    gst_wl_buffer_attach (buffer, window->video_surface_wrapper);
    wl_surface_set_buffer_scale (window->video_surface_wrapper, window->scale);
    wl_surface_damage_buffer (window->video_surface_wrapper, 0, 0, G_MAXINT32,
        G_MAXINT32);
    wl_surface_commit (window->video_surface_wrapper);

    if (!window->is_area_surface_mapped) {
      gst_wl_window_update_borders (window);
      wl_surface_commit (window->area_surface_wrapper);
      window->is_area_surface_mapped = TRUE;
    }
  } else {
    /* clear both video and parent surfaces */
    wl_surface_attach (window->video_surface_wrapper, NULL, 0, 0);
    wl_surface_set_buffer_scale (window->video_surface_wrapper, window->scale);
    wl_surface_commit (window->video_surface_wrapper);
    wl_surface_attach (window->area_surface_wrapper, NULL, 0, 0);
    wl_surface_commit (window->area_surface_wrapper);
    window->is_area_surface_mapped = FALSE;
  }

  if (G_UNLIKELY (info)) {
    /* commit also the parent (area_surface) in order to change
     * the position of the video_subsurface */
    wl_surface_commit (window->area_surface_wrapper);
    wl_subsurface_set_desync (window->video_subsurface);
  }

  wl_display_flush (window->display->display);
}

/* Update the buffer used to draw black borders. When we have viewporter
 * support, this is a scaled up 1x1 image, and without we need an black image
 * the size of the rendering areay. */
static void
gst_wl_window_update_borders (GstWlWindow * window)
{
  GstVideoFormat format;
  GstVideoInfo info;
  gint width, height;
  GstBuffer *buf;
  struct wl_buffer *wlbuf;
  GstWlBuffer *gwlbuf;
  GstAllocator *alloc;

  if (window->display->viewporter) {
    wp_viewport_set_destination (window->area_viewport,
        window->render_rectangle.w, window->render_rectangle.h);

    if (window->is_area_surface_mapped) {
      /* The area_surface is already visible and only needed to get resized.
       * We don't need to attach a new buffer and are done here. */
      return;
    }
  }

  if (window->display->viewporter) {
    width = height = 1;
  } else {
    width = window->render_rectangle.w;
    height = window->render_rectangle.h;
  }

  /* we want WL_SHM_FORMAT_XRGB8888 */
  format = GST_VIDEO_FORMAT_BGRx;

  /* draw the area_subsurface */
  gst_video_info_set_format (&info, format, width, height);

  alloc = gst_wl_shm_allocator_get ();

  buf = gst_buffer_new_allocate (alloc, info.size, NULL);
  gst_buffer_memset (buf, 0, 0, info.size);
  wlbuf =
      gst_wl_shm_memory_construct_wl_buffer (gst_buffer_peek_memory (buf, 0),
      window->display, &info);
  gwlbuf = gst_buffer_add_wl_buffer (buf, wlbuf, window->display);
  gst_wl_buffer_attach (gwlbuf, window->area_surface_wrapper);
  wl_surface_damage_buffer (window->area_surface_wrapper, 0, 0, G_MAXINT32,
      G_MAXINT32);

  /* at this point, the GstWlBuffer keeps the buffer
   * alive and will free it on wl_buffer::release */
  gst_buffer_unref (buf);
  g_object_unref (alloc);
}

void
gst_wl_window_set_render_rectangle (GstWlWindow * window, gint x, gint y,
    gint w, gint h)
{
  g_return_if_fail (window != NULL);

  if (window->render_rectangle.x == x && window->render_rectangle.y == y &&
      window->render_rectangle.w == w && window->render_rectangle.h == h)
    return;

  window->render_rectangle.x = x;
  window->render_rectangle.y = y;
  window->render_rectangle.w = w;
  window->render_rectangle.h = h;

  /* position the area inside the parent - needs a parent commit to apply */
  if (window->area_subsurface)
    wl_subsurface_set_position (window->area_subsurface, x, y);

  if (window->is_area_surface_mapped)
    gst_wl_window_update_borders (window);

  if (!window->configured)
    return;

  if (window->video_width != 0) {
    wl_subsurface_set_sync (window->video_subsurface);
    gst_wl_window_resize_video_surface (window, TRUE);
  }

  wl_surface_commit (window->area_surface_wrapper);

  if (window->video_width != 0)
    wl_subsurface_set_desync (window->video_subsurface);
}

void
gst_wl_window_set_source_crop (GstWlWindow * window, GstBuffer * buffer)
{
  GstVideoCropMeta *crop = NULL;
  crop = gst_buffer_get_video_crop_meta (buffer);

  if (crop) {
    GST_DEBUG ("buffer crop x=%d y=%d width=%d height=%d\n",
        crop->x, crop->y, crop->width, crop->height);
    window->src_x = crop->x;
    window->src_y = crop->y;
    window->src_width = crop->width;
    window->src_height = crop->height;
  } else {
    window->src_width = -1;
  }
}

void
gst_wl_window_set_alpha (GstWlWindow * window, gfloat alpha)
{
  if (window && window->blend_func) {
    zwp_blending_v1_set_alpha (window->blend_func,
        wl_fixed_from_double (alpha));
    if (alpha < 1.0)
      zwp_blending_v1_set_blending (window->blend_func,
          ZWP_BLENDING_V1_BLENDING_EQUATION_FROMSOURCE);
    else
      zwp_blending_v1_set_blending (window->blend_func,
          ZWP_BLENDING_V1_BLENDING_EQUATION_PREMULTIPLIED);
  }
}
