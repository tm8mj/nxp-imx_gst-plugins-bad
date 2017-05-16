/*
 * GStreamer
 * Copyright (C) 2012 Matthew Waters <ystree00@gmail.com>
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gl/gl.h>
#include "gstgldownloadelement.h"

#if GST_GL_HAVE_PHYMEM
#include <gst/gl/gstglphymemory.h>
#endif

GST_DEBUG_CATEGORY_STATIC (gst_gl_download_element_debug);
#define GST_CAT_DEFAULT gst_gl_download_element_debug

#define gst_gl_download_element_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstGLDownloadElement, gst_gl_download_element,
    GST_TYPE_GL_BASE_FILTER,
    GST_DEBUG_CATEGORY_INIT (gst_gl_download_element_debug, "gldownloadelement",
        0, "download element"););

static gboolean gst_gl_download_element_get_unit_size (GstBaseTransform * trans,
    GstCaps * caps, gsize * size);
static GstCaps *gst_gl_download_element_transform_caps (GstBaseTransform * bt,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static gboolean gst_gl_download_element_set_caps (GstBaseTransform * bt,
    GstCaps * in_caps, GstCaps * out_caps);
static GstFlowReturn
gst_gl_download_element_prepare_output_buffer (GstBaseTransform * bt,
    GstBuffer * buffer, GstBuffer ** outbuf);
static GstFlowReturn gst_gl_download_element_transform (GstBaseTransform * bt,
    GstBuffer * buffer, GstBuffer * outbuf);
static gboolean gst_gl_download_element_propose_allocation (GstBaseTransform *
    bt, GstQuery * decide_query, GstQuery * query);

static GstStaticPadTemplate gst_gl_download_element_src_pad_template =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw; video/x-raw(memory:GLMemory)"));

static GstStaticPadTemplate gst_gl_download_element_sink_pad_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw(memory:GLMemory); video/x-raw"));

static void
gst_gl_download_element_class_init (GstGLDownloadElementClass * klass)
{
  GstBaseTransformClass *bt_class = GST_BASE_TRANSFORM_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  bt_class->transform_caps = gst_gl_download_element_transform_caps;
  bt_class->set_caps = gst_gl_download_element_set_caps;
  bt_class->get_unit_size = gst_gl_download_element_get_unit_size;
  bt_class->prepare_output_buffer =
      gst_gl_download_element_prepare_output_buffer;
  bt_class->transform = gst_gl_download_element_transform;
  bt_class->propose_allocation = gst_gl_download_element_propose_allocation;

  bt_class->passthrough_on_same_caps = TRUE;

  gst_element_class_add_static_pad_template (element_class,
      &gst_gl_download_element_src_pad_template);
  gst_element_class_add_static_pad_template (element_class,
      &gst_gl_download_element_sink_pad_template);

  gst_element_class_set_metadata (element_class,
      "OpenGL downloader", "Filter/Video",
      "Downloads data from OpenGL", "Matthew Waters <matthew@centricular.com>");
}

static void
gst_gl_download_element_init (GstGLDownloadElement * download)
{
  gst_base_transform_set_prefer_passthrough (GST_BASE_TRANSFORM (download),
      TRUE);
}

static gboolean
gst_gl_download_element_set_caps (GstBaseTransform * bt, GstCaps * in_caps,
    GstCaps * out_caps)
{
  GstVideoInfo out_info;

  if (!gst_video_info_from_caps (&out_info, out_caps))
    return FALSE;

  return TRUE;
}

static GstCaps *
_set_caps_features (const GstCaps * caps, const gchar * feature_name)
{
  GstCaps *tmp = gst_caps_copy (caps);
  guint n = gst_caps_get_size (tmp);
  guint i = 0;

  for (i = 0; i < n; i++)
    gst_caps_set_features (tmp, i,
        gst_caps_features_from_string (feature_name));

  return tmp;
}

static GstCaps *
gst_gl_download_element_transform_caps (GstBaseTransform * bt,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *result, *tmp;

  if (direction == GST_PAD_SRC) {
    tmp = _set_caps_features (caps, GST_CAPS_FEATURE_MEMORY_GL_MEMORY);
    tmp = gst_caps_merge (gst_caps_ref (caps), tmp);
  } else {
    tmp = _set_caps_features (caps, GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY);
    tmp = gst_caps_merge (gst_caps_ref (caps), tmp);
  }

  if (filter) {
    result = gst_caps_intersect_full (filter, tmp, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (tmp);
  } else {
    result = tmp;
  }

  GST_DEBUG_OBJECT (bt, "returning caps %" GST_PTR_FORMAT, result);

  return result;
}

static gboolean
gst_gl_download_element_get_unit_size (GstBaseTransform * trans, GstCaps * caps,
    gsize * size)
{
  gboolean ret = FALSE;
  GstVideoInfo info;

  ret = gst_video_info_from_caps (&info, caps);
  if (ret)
    *size = GST_VIDEO_INFO_SIZE (&info);

  return TRUE;
}

static GstFlowReturn
gst_gl_download_element_prepare_output_buffer (GstBaseTransform * bt,
    GstBuffer * inbuf, GstBuffer ** outbuf)
{
  GstGLDownloadElement *download = GST_GL_DOWNLOAD_ELEMENT (bt);
  GstCaps *src_caps = gst_pad_get_current_caps (bt->srcpad);
  GstCapsFeatures *features = NULL;
  gint i, n;
  GstGLMemory *glmem;

#if GST_GL_HAVE_PHYMEM
  glmem = gst_buffer_peek_memory (inbuf, 0);
  if (gst_is_gl_physical_memory (glmem)) {
    GstGLContext *context = GST_GL_BASE_FILTER (bt)->context;
    GstVideoInfo info;

    gst_video_info_from_caps (&info, src_caps);
    *outbuf = gst_gl_phymem_buffer_to_gstbuffer (context, &info, inbuf);

    GST_DEBUG_OBJECT (download, "gl download with direct viv.");

    return GST_FLOW_OK;
  }
#endif /* GST_GL_HAVE_PHYMEM */

  *outbuf = inbuf;

  if (src_caps)
    features = gst_caps_get_features (src_caps, 0);

  n = gst_buffer_n_memory (*outbuf);
  for (i = 0; i < n; i++) {
    GstMemory *mem = gst_buffer_peek_memory (*outbuf, i);

    if (gst_is_gl_memory (mem)) {
      if (!features || gst_caps_features_contains (features,
              GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY)) {
        if (gst_is_gl_memory_pbo (mem))
          gst_gl_memory_pbo_download_transfer ((GstGLMemoryPBO *) mem);
      }
    }
  }

  if (src_caps)
    gst_caps_unref (src_caps);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_gl_download_element_transform (GstBaseTransform * bt,
    GstBuffer * inbuf, GstBuffer * outbuf)
{
  return GST_FLOW_OK;
}

static gboolean
gst_gl_download_element_propose_allocation (GstBaseTransform * bt,
    GstQuery * decide_query, GstQuery * query)
{
  GstGLContext *context = GST_GL_BASE_FILTER (bt)->context;
  GstGLDownloadElement *download = GST_GL_DOWNLOAD_ELEMENT (bt);
  GstAllocationParams params;
  GstAllocator *allocator = NULL;
  GstBufferPool *pool = NULL;
  guint n_pools, i;
  GstVideoInfo info;
  GstCaps *caps;
  GstStructure *config;
  gsize size;

  gst_query_parse_allocation (query, &caps, NULL);
  if (!gst_video_info_from_caps (&info, caps)) {
    GST_WARNING_OBJECT (bt, "invalid caps specified");
    return FALSE;
  }

  GST_DEBUG_OBJECT (bt, "video format is %s", gst_video_format_to_string (GST_VIDEO_INFO_FORMAT (&info)));

  gst_allocation_params_init (&params);

#if GST_GL_HAVE_PHYMEM
  if (gst_is_gl_physical_memory_supported_fmt (&info)) {
    allocator = gst_phy_mem_allocator_obtain ();
    GST_DEBUG_OBJECT (bt, "obtain physical memory allocator %p.", allocator);
  }
#endif /* GST_GL_HAVE_PHYMEM */

  if (!allocator)
    allocator = gst_allocator_find (GST_GL_MEMORY_ALLOCATOR_NAME);

  if (!allocator) {
    GST_ERROR_OBJECT (bt, "Can't obtain gl memory allocator.");
    return FALSE;
  }

  gst_query_add_allocation_param (query, allocator, &params);
  gst_object_unref (allocator);

  n_pools = gst_query_get_n_allocation_pools (query);
  for (i = 0; i < n_pools; i++) {
    gst_query_parse_nth_allocation_pool (query, i, &pool, NULL, NULL, NULL);
    gst_object_unref (pool);
    pool = NULL;
  }

  //new buffer pool
  pool = gst_gl_buffer_pool_new (context);
  config = gst_buffer_pool_get_config (pool);

  /* the normal size of a frame */
  size = info.size;
  gst_buffer_pool_config_set_params (config, caps, size, 0, 0);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_GL_SYNC_META);

  if (!gst_buffer_pool_set_config (pool, config)) {
    gst_object_unref (pool);
    GST_WARNING_OBJECT (bt, "failed setting config");
    return FALSE;
  }

  GST_DEBUG_OBJECT (download, "create pool %p", pool);

  //propose 3 buffers for better performance
  gst_query_add_allocation_pool (query, pool, size, 3, 0);

  gst_object_unref (pool);

  return TRUE;
}
