/* GStreamer
 * Copyright (C) <2020> NXP, Bing Song <bing.song@nxp.com>.
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

/**
 * SECTION:element-spdifdemux
 *
 * Parse iec937 into compressed audio.
 *
 * Spdifdemux supports both push and pull mode operations, making it possible to
 * stream from a network source.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 filesrc location=sine.937 ! spdifdemux ! audioconvert ! alsasink
 * ]| Read a iec937 file and output to the soundcard using the ALSA element. The
 * iec937 file is assumed to contain compressed audio.
 * |[
 * gst-launch-1.0 alsasrc ! queue ! spdifdemux ! decodebin ! alsasink
 * ]| Playback data from a alsasrc.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <math.h>

#include "gstspdifdemux.h"

GST_DEBUG_CATEGORY_STATIC (spdifdemux_debug);
#define GST_CAT_DEFAULT (spdifdemux_debug)

#define IEC937_PA (0x72)
#define IEC937_PAPB (0xF872)
#define IEC937_PCPD (0x4E1F)

typedef enum
{
  IEC937_FORMAT_TYPE_AC3 = 0x01,
  IEC937_FORMAT_TYPE_EAC3 = 0x15,
  IEC937_FORMAT_TYPE_MPEG1L1 = 0x4,
  IEC937_FORMAT_TYPE_MPEG1L23 = 0x5,
  IEC937_FORMAT_TYPE_MPEG2 = 0x6,
  IEC937_FORMAT_TYPE_MPEG2L1 = 0x8,
  IEC937_FORMAT_TYPE_MPEG2L2 = 0x9,
  IEC937_FORMAT_TYPE_MPEG2L3 = 0xA,
  IEC937_FORMAT_TYPE_MPEG2_4_AAC = 0x7,
  IEC937_FORMAT_TYPE_MPEG2_4_AAC_2 = 0x13,
  IEC937_FORMAT_TYPE_MPEG2_4_AAC_3 = 0x33
} GstIec937FormatType;

static void gst_spdifdemux_dispose (GObject * object);

static gboolean gst_spdifdemux_sink_activate (GstPad * sinkpad,
    GstObject * parent);
static gboolean gst_spdifdemux_sink_activate_mode (GstPad * sinkpad,
    GstObject * parent, GstPadMode mode, gboolean active);
static GstStateChangeReturn gst_spdifdemux_change_state (GstElement * element,
    GstStateChange transition);

static gboolean gst_spdifdemux_pad_query (GstPad * pad, GstObject * parent,
    GstQuery * query);

static GstFlowReturn gst_spdifdemux_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buf);
static gboolean gst_spdifdemux_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static void gst_spdifdemux_loop (GstPad * pad);
static gboolean gst_spdifdemux_srcpad_event (GstPad * pad, GstObject * parent,
    GstEvent * event);

static void gst_spdifdemux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_spdifdemux_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

#define DEFAULT_IGNORE_LENGTH FALSE

enum
{
  PROP_0,
  PROP_IGNORE_LENGTH,
};

static GstStaticPadTemplate sink_template_factory =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-raw, "
        "format = (string) " GST_AUDIO_FORMATS_ALL ", "
        "layout = (string) interleaved, "
        "rate = (int) [ 1, MAX ], " "channels = (int) [ 1, MAX ]")
    );

static GstStaticPadTemplate src_template_factory =
    GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("audio/x-ac3;"
        "audio/x-eac3; "
        "audio/mpeg, mpegversion = (int) 1; "
        "audio/mpeg, mpegversion = (int) { 2, 4 }; ")
    );


#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (spdifdemux_debug, "spdifdemux", 0, "SPDIF demuxer");

#define gst_spdifdemux_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstSpdifDemux, gst_spdifdemux, GST_TYPE_ELEMENT,
    DEBUG_INIT);

static void
gst_spdifdemux_class_init (GstSpdifDemuxClass * klass)
{
  GstElementClass *gstelement_class;
  GObjectClass *object_class;

  gstelement_class = (GstElementClass *) klass;
  object_class = (GObjectClass *) klass;

  parent_class = g_type_class_peek_parent (klass);

  object_class->dispose = gst_spdifdemux_dispose;

  object_class->set_property = gst_spdifdemux_set_property;
  object_class->get_property = gst_spdifdemux_get_property;

  /**
   * GstSpdifDemux:ignore-length:
   *
   * This selects whether the length found in a data chunk
   * should be ignored. This may be useful for streamed audio
   * where the length is unknown until the end of streaming,
   * and various software/hardware just puts some random value
   * in there and hopes it doesn't break too much.
   */
  g_object_class_install_property (object_class, PROP_IGNORE_LENGTH,
      g_param_spec_boolean ("ignore-length",
          "Ignore length",
          "Ignore length from the zeros at the begin",
          DEFAULT_IGNORE_LENGTH, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
      );

  gstelement_class->change_state = gst_spdifdemux_change_state;

  /* register pads */
  gst_element_class_add_static_pad_template (gstelement_class,
      &sink_template_factory);
  gst_element_class_add_static_pad_template (gstelement_class,
      &src_template_factory);

  gst_element_class_set_static_metadata (gstelement_class, "SPDIF demuxer",
      "Codec/Demuxer/Audio",
      "Parse a iec937 file into compressed audio",
      "Bing Song <bing.song@nxp.com>");
}

static void
gst_spdifdemux_reset (GstSpdifDemux * spdif)
{
  spdif->state = GST_SPDIFDEMUX_HEADER;

  /* These will all be set correctly in the fmt chunk */
  spdif->depth = 0;
  spdif->rate = 0;
  spdif->width = 0;
  spdif->channels = 0;
  spdif->blockalign = 0;
  spdif->bps = 0;
  spdif->fact = 0;
  spdif->offset = 0;
  spdif->end_offset = 0;
  spdif->dataleft = 0;
  spdif->datasize = 0;
  spdif->datastart = 0;
  spdif->duration = 0;
  spdif->got_fmt = FALSE;
  spdif->first = TRUE;

  if (spdif->adapter) {
    gst_adapter_clear (spdif->adapter);
    g_object_unref (spdif->adapter);
    spdif->adapter = NULL;
  }
  if (spdif->caps)
    gst_caps_unref (spdif->caps);
  spdif->caps = NULL;
  if (spdif->start_segment)
    gst_event_unref (spdif->start_segment);
  spdif->start_segment = NULL;
}

static void
gst_spdifdemux_dispose (GObject * object)
{
  GstSpdifDemux *spdif = GST_SPDIFDEMUX (object);

  GST_DEBUG_OBJECT (spdif, "SPDIF: Dispose");
  gst_spdifdemux_reset (spdif);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_spdifdemux_init (GstSpdifDemux * spdifdemux)
{
  gst_spdifdemux_reset (spdifdemux);

  /* sink */
  spdifdemux->sinkpad =
      gst_pad_new_from_static_template (&sink_template_factory, "sink");
  gst_pad_set_activate_function (spdifdemux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_spdifdemux_sink_activate));
  gst_pad_set_activatemode_function (spdifdemux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_spdifdemux_sink_activate_mode));
  gst_pad_set_chain_function (spdifdemux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_spdifdemux_chain));
  gst_pad_set_event_function (spdifdemux->sinkpad,
      GST_DEBUG_FUNCPTR (gst_spdifdemux_sink_event));
  gst_element_add_pad (GST_ELEMENT_CAST (spdifdemux), spdifdemux->sinkpad);

  /* src */
  spdifdemux->srcpad =
      gst_pad_new_from_static_template (&src_template_factory, "src");
  gst_pad_set_query_function (spdifdemux->srcpad,
      GST_DEBUG_FUNCPTR (gst_spdifdemux_pad_query));
  gst_pad_set_event_function (spdifdemux->srcpad,
      GST_DEBUG_FUNCPTR (gst_spdifdemux_srcpad_event));
  gst_element_add_pad (GST_ELEMENT_CAST (spdifdemux), spdifdemux->srcpad);
}

static gboolean
gst_spdifdemux_map_type (GstSpdifDemux * spdif, GstIec937FormatType type)
{
  gint rate, channels;

  switch (type) {
    case IEC937_FORMAT_TYPE_AC3:
    {
      spdif->caps = gst_caps_new_empty_simple ("audio/x-ac3");
      break;
    }
    case IEC937_FORMAT_TYPE_EAC3:
    {
      spdif->caps =
          gst_caps_new_simple ("audio/x-eac3", "alignment", G_TYPE_STRING,
          "iec61937", NULL);
      break;
    }
    case IEC937_FORMAT_TYPE_MPEG1L1:
    {
      spdif->caps =
          gst_caps_new_simple ("audio/mpeg", "mpegversion", G_TYPE_INT, 1,
          "mpegaudioversion", G_TYPE_INT, 1, "layer", G_TYPE_INT, 1, NULL);
    }
    case IEC937_FORMAT_TYPE_MPEG2L1:
    {
      spdif->caps =
          gst_caps_new_simple ("audio/mpeg", "mpegversion", G_TYPE_INT, 1,
          "mpegaudioversion", G_TYPE_INT, 2, "layer", G_TYPE_INT, 1, NULL);
    }
    case IEC937_FORMAT_TYPE_MPEG2L2:
    {
      spdif->caps =
          gst_caps_new_simple ("audio/mpeg", "mpegversion", G_TYPE_INT, 1,
          "mpegaudioversion", G_TYPE_INT, 2, "layer", G_TYPE_INT, 2, NULL);
    }
    case IEC937_FORMAT_TYPE_MPEG2:
    case IEC937_FORMAT_TYPE_MPEG1L23:
    case IEC937_FORMAT_TYPE_MPEG2L3:
    {
      spdif->caps =
          gst_caps_new_simple ("audio/mpeg", "mpegversion", G_TYPE_INT, 1,
          "mpegaudioversion", G_TYPE_INT, 2, "layer", G_TYPE_INT, 3, NULL);
    }
      break;
    case IEC937_FORMAT_TYPE_MPEG2_4_AAC:
    case IEC937_FORMAT_TYPE_MPEG2_4_AAC_2:
    case IEC937_FORMAT_TYPE_MPEG2_4_AAC_3:
    {
      spdif->caps =
          gst_caps_new_simple ("audio/mpeg", "mpegversion", G_TYPE_INT, 2,
          "stream-format", G_TYPE_STRING, "adts", NULL);
      break;
    }
    default:
      GST_ERROR_OBJECT (spdif, "Unkonw format!!!");
      return FALSE;
  }

  gst_caps_set_simple (spdif->caps, "channels", G_TYPE_INT,
      spdif->spec.info.channels, "rate", G_TYPE_INT, spdif->spec.info.rate,
      NULL);

  GST_DEBUG_OBJECT (spdif, "sink caps %" GST_PTR_FORMAT, spdif->caps);
  spdif->spec_out.latency_time = GST_SECOND;
  if (!gst_audio_ring_buffer_parse_caps (&spdif->spec_out, spdif->caps)) {
    GST_ERROR_OBJECT (spdif, "failed to get structure from caps");
    return FALSE;
  }

  GST_DEBUG_OBJECT (spdif, "sink caps %" GST_PTR_FORMAT, spdif->caps);

  return TRUE;
}

static gboolean
gst_spdifdemux_parse_format (GstSpdifDemux * spdif)
{
  const guint8 *data = NULL;
  GstIec937FormatType type;

  /* Pc: bit 13-15 - stream number (0)
   *     bit 11-12 - reserved (0)
   *     bit  8-10 - bsmod from AC3 frame */
  /* Pc: bit    7  - error bit (0)
   *     bit  5-6  - subdata type (0)
   *     bit  0-4  - data type (1) */

  if (gst_adapter_available (spdif->adapter) < 8)
    return FALSE;

  data = gst_adapter_map (spdif->adapter, 8);
  type = *(data + 4) & 0x1F;
  gst_adapter_unmap (spdif->adapter);

  return gst_spdifdemux_map_type (spdif, type);
}

static GstFlowReturn
gst_spdifdemux_parse_format_file (GstSpdifDemux * spdif)
{
  GstFlowReturn res = GST_FLOW_OK;
  GstIec937FormatType type;
  GstBuffer *buf = NULL;
  GstMapInfo map;

  if ((res = gst_pad_pull_range (spdif->sinkpad, spdif->offset,
              8, &buf)) != GST_FLOW_OK)
    goto pull_error;

  /* Pc: bit 13-15 - stream number (0)
   *     bit 11-12 - reserved (0)
   *     bit  8-10 - bsmod from AC3 frame */
  /* Pc: bit    7  - error bit (0)
   *     bit  5-6  - subdata type (0)
   *     bit  0-4  - data type (1) */

  gst_buffer_map (buf, &map, GST_MAP_READ);
  type = *(map.data + 4) & 0x1F;
  gst_buffer_unmap (buf, &map);

  if (!gst_spdifdemux_map_type (spdif, type))
    return GST_FLOW_ERROR;

  return res;

  /* ERROR */
found_eos:
  {
    GST_DEBUG_OBJECT (spdif, "found EOS");
    return GST_FLOW_EOS;
  }
pull_error:
  {
    /* check if we got EOS */
    if (res == GST_FLOW_EOS)
      goto found_eos;

    GST_ERROR_OBJECT (spdif, "Pull error!");
    return res;
  }
}

static gboolean
gst_spdifdemux_check_sync_word (GstSpdifDemux * spdif)
{
  const guint8 *data = NULL;

  if (gst_adapter_available (spdif->adapter) < 8)
    return FALSE;

  data = gst_adapter_map (spdif->adapter, 8);
  if (GST_READ_UINT16_LE (data) == IEC937_PAPB
      && GST_READ_UINT16_LE (data + 2) == IEC937_PCPD) {
    GST_DEBUG_OBJECT (spdif, "Found sync word in: 0x%x", spdif->offset);
    gst_adapter_unmap (spdif->adapter);
    return TRUE;
  }
  gst_adapter_unmap (spdif->adapter);
  gst_adapter_flush (spdif->adapter, 1);
  spdif->offset += 1;

  return FALSE;
}

static GstFlowReturn
gst_spdifdemux_check_sync_word_file (GstSpdifDemux * spdif)
{
  GstFlowReturn res = GST_FLOW_OK;
  GstBuffer *buf = NULL;
  GstMapInfo map;

  if ((res = gst_pad_pull_range (spdif->sinkpad, spdif->offset,
              8, &buf)) != GST_FLOW_OK)
    goto pull_error;

  gst_buffer_map (buf, &map, GST_MAP_READ);
  if (GST_READ_UINT16_LE (map.data) == IEC937_PAPB
      && GST_READ_UINT16_LE (map.data + 2) == IEC937_PCPD) {
    GST_DEBUG_OBJECT (spdif, "Found sync word in: 0x%x", spdif->offset);
    gst_buffer_unmap (buf, &map);
    return res;
  }
  gst_buffer_unmap (buf, &map);
  spdif->offset += 1;

  return GST_FLOW_ERROR;

  /* ERROR */
found_eos:
  {
    GST_DEBUG_OBJECT (spdif, "found EOS");
    return GST_FLOW_EOS;
  }
pull_error:
  {
    /* check if we got EOS */
    if (res == GST_FLOW_EOS)
      goto found_eos;

    GST_ERROR_OBJECT (spdif, "Pull error!");
    return res;
  }
}

static gboolean
gst_spdifdemux_search_pa (GstSpdifDemux * spdif)
{
  const guint8 *data = NULL;

  while (gst_adapter_available (spdif->adapter) > 0) {
    data = gst_adapter_map (spdif->adapter, 1);
    if (data[0] == IEC937_PA) {
      GST_DEBUG_OBJECT (spdif, "Found PA in: 0x%x", spdif->offset);
      gst_adapter_unmap (spdif->adapter);
      return TRUE;
    }
    gst_adapter_unmap (spdif->adapter);
    gst_adapter_flush (spdif->adapter, 1);
    spdif->offset += 1;
  }

  return FALSE;
}

static GstFlowReturn
gst_spdifdemux_search_pa_file (GstSpdifDemux * spdif)
{
  GstFlowReturn res = GST_FLOW_OK;
  GstBuffer *buf = NULL;
  GstMapInfo map;

  while (TRUE) {
    if ((res = gst_pad_pull_range (spdif->sinkpad, spdif->offset,
                1, &buf)) != GST_FLOW_OK)
      goto pull_error;

    gst_buffer_map (buf, &map, GST_MAP_READ);
    if (*(map.data) == IEC937_PA) {
      GST_DEBUG_OBJECT (spdif, "Found PA in: 0x%x", spdif->offset);
      gst_buffer_unmap (buf, &map);
      return res;
    }
    gst_buffer_unmap (buf, &map);
    spdif->offset += 1;
  }

  return GST_FLOW_ERROR;

  /* ERROR */
found_eos:
  {
    GST_DEBUG_OBJECT (spdif, "found EOS");
    return GST_FLOW_EOS;
  }
pull_error:
  {
    /* check if we got EOS */
    if (res == GST_FLOW_EOS)
      goto found_eos;

    GST_ERROR_OBJECT (spdif, "Pull error!");
    return res;
  }
}

static GstFlowReturn
gst_spdifdemux_stream_headers (GstSpdifDemux * spdif)
{
  GstFlowReturn res = GST_FLOW_OK;

  if (spdif->streaming) {
    if (!gst_spdifdemux_search_pa (spdif))
      return res;

    if (!gst_spdifdemux_check_sync_word (spdif))
      return res;

    if (!gst_spdifdemux_parse_format (spdif))
      return GST_FLOW_ERROR;
  } else {
    if ((res = gst_spdifdemux_search_pa_file (spdif)) != GST_FLOW_OK)
      return res;

    if ((res = gst_spdifdemux_check_sync_word_file (spdif)) != GST_FLOW_OK)
      return GST_FLOW_OK;

    if ((res = gst_spdifdemux_parse_format_file (spdif)) != GST_FLOW_OK)
      return res;
  }

  spdif->got_fmt = TRUE;

  return GST_FLOW_OK;
}

static void
gst_spdifdemux_add_src_pad (GstSpdifDemux * spdif, GstBuffer * buf)
{
  GST_DEBUG_OBJECT (spdif, "adding src pad");

  g_assert (spdif->caps != NULL);

  GST_DEBUG_OBJECT (spdif, "sending caps %" GST_PTR_FORMAT, spdif->caps);
  gst_pad_set_caps (spdif->srcpad, spdif->caps);

  if (spdif->start_segment) {
    GST_DEBUG_OBJECT (spdif, "Send start segment event on newpad");
    gst_pad_push_event (spdif->srcpad, spdif->start_segment);
    spdif->start_segment = NULL;
  }
}

static guint32
gst_spdifdemux_get_stream_len (GstSpdifDemux * spdif, GstBuffer * buf)
{
  GstMapInfo map;
  guint32 stream_len;

  /* Pd: bit 15-0  - frame size in bits */

  gst_buffer_map (buf, &map, GST_MAP_READ);
  /* EAC3 is frame size in bytes */
  if (spdif->spec_out.type == GST_AUDIO_RING_BUFFER_FORMAT_TYPE_EAC3)
    stream_len = GST_READ_UINT16_LE (map.data + 6);
  else
    stream_len = GST_READ_UINT16_LE (map.data + 6) >> 3;
  gst_buffer_unmap (buf, &map);

  GST_DEBUG_OBJECT (spdif, "iec937 stream size: %d", stream_len);
  return stream_len;
}

static gboolean
gst_spdifdemux_swap_stream (GstBuffer * buf, guint offset, guint stream_size)
{
  GstMapInfo map;
  guint16 *p16, i;

  gst_buffer_map (buf, &map, GST_MAP_READ | GST_MAP_WRITE);
  p16 = map.data;
  for (i = offset; i < stream_size + offset; i += 2) {
    p16[i >> 1] = GST_READ_UINT16_BE (map.data + i);
  }
  gst_buffer_unmap (buf, &map);

  return TRUE;
}

static GstFlowReturn
gst_spdifdemux_stream_data (GstSpdifDemux * spdif, gboolean flushing)
{
  GstBuffer *buf = NULL;
  GstFlowReturn res = GST_FLOW_OK;
  GstClockTime timestamp, next_timestamp, duration;
  guint64 pos, nextpos;
  guint obtained, framesize, stream_size;

iterate_adapter:

  if (spdif->streaming) {
    if (!gst_spdifdemux_check_sync_word (spdif)) {
      GST_WARNING_OBJECT (spdif, "Lost sync!!!. Try to resync.");
      gst_spdifdemux_search_pa (spdif);
      return GST_FLOW_OK;
    }
  }

  framesize = gst_audio_iec61937_frame_size (&spdif->spec_out);

  GST_DEBUG_OBJECT (spdif, "iec937 frame size: %d position: %lld", framesize,
      spdif->offset);
  if (spdif->streaming) {
    guint avail = gst_adapter_available (spdif->adapter);
    if (avail < framesize) {
      GST_DEBUG_OBJECT (spdif, "Got only %u bytes of data from the sinkpad",
          avail);
      return GST_FLOW_OK;
    } else {
      buf = gst_adapter_take_buffer (spdif->adapter, framesize);
    }
  } else {
    if ((res = gst_pad_pull_range (spdif->sinkpad, spdif->offset,
                framesize, &buf)) != GST_FLOW_OK)
      goto pull_error;

    /* we may get a short buffer at the end of the file */
    if (gst_buffer_get_size (buf) < framesize) {
      gsize size = gst_buffer_get_size (buf);

      GST_LOG_OBJECT (spdif, "Got only %" G_GSIZE_FORMAT " bytes of data",
          size);
      gst_buffer_unref (buf);
      goto found_eos;
    }
  }

  obtained = gst_buffer_get_size (buf);
  buf = gst_buffer_make_writable (buf);
  stream_size = gst_spdifdemux_get_stream_len (spdif, buf);
  if (stream_size > obtained) {
    GST_ERROR_OBJECT (spdif, "Stream size: %d bigger then buffer size: %d!",
        stream_size, obtained);
    return GST_FLOW_OK;
  }
  gst_spdifdemux_swap_stream (buf, 8, stream_size);
  gst_buffer_resize (buf, 8, stream_size);

  /* our positions in bytes */
  pos = spdif->offset - spdif->datastart;
  nextpos = pos + obtained;

  /* update offsets, does not overflow. */
  GST_BUFFER_OFFSET (buf) = pos / spdif->bytes_per_sample;
  GST_BUFFER_OFFSET_END (buf) = nextpos / spdif->bytes_per_sample;

  /* first chunk of data? create the source pad. We do this only here so
   * we can detect broken .spdif files with dts disguised as raw PCM (sigh) */
  if (G_UNLIKELY (spdif->first)) {
    spdif->first = FALSE;
    /* this will also push the segment events */
    gst_spdifdemux_add_src_pad (spdif, buf);
  } else {
    /* If we have a pending start segment, send it now. */
    if (G_UNLIKELY (spdif->start_segment != NULL)) {
      gst_pad_push_event (spdif->srcpad, spdif->start_segment);
      spdif->start_segment = NULL;
    }
  }

  if (spdif->bps > 0) {
    /* and timestamps if we have a bitrate, be careful for overflows */
    timestamp =
        gst_util_uint64_scale_ceil (pos, GST_SECOND, (guint64) spdif->bps);
    next_timestamp =
        gst_util_uint64_scale_ceil (nextpos, GST_SECOND, (guint64) spdif->bps);
    duration = next_timestamp - timestamp;

    /* update current running segment position */
    if (G_LIKELY (next_timestamp >= spdif->segment.start))
      spdif->segment.position = next_timestamp;
  } else if (spdif->fact) {
    guint64 bps =
        gst_util_uint64_scale_int (spdif->datasize, spdif->rate, spdif->fact);
    /* and timestamps if we have a bitrate, be careful for overflows */
    timestamp = gst_util_uint64_scale_ceil (pos, GST_SECOND, bps);
    next_timestamp = gst_util_uint64_scale_ceil (nextpos, GST_SECOND, bps);
    duration = next_timestamp - timestamp;
  } else {
    /* no bitrate, all we know is that the first sample has timestamp 0, all
     * other positions and durations have unknown timestamp. */
    if (pos == 0)
      timestamp = 0;
    else
      timestamp = GST_CLOCK_TIME_NONE;
    duration = GST_CLOCK_TIME_NONE;
    /* update current running segment position with byte offset */
    if (G_LIKELY (nextpos >= spdif->segment.start))
      spdif->segment.position = nextpos;
  }
  if ((pos > 0) && spdif->vbr) {
    /* don't set timestamps for VBR files if it's not the first buffer */
    timestamp = GST_CLOCK_TIME_NONE;
    duration = GST_CLOCK_TIME_NONE;
  }
  if (spdif->discont) {
    GST_DEBUG_OBJECT (spdif, "marking DISCONT");
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
    spdif->discont = FALSE;
  }

  GST_BUFFER_TIMESTAMP (buf) = timestamp;
  GST_BUFFER_DURATION (buf) = duration;

  GST_LOG_OBJECT (spdif,
      "Got buffer. timestamp:%" GST_TIME_FORMAT " , duration:%" GST_TIME_FORMAT
      ", size:%" G_GSIZE_FORMAT, GST_TIME_ARGS (timestamp),
      GST_TIME_ARGS (duration), gst_buffer_get_size (buf));

  if ((res = gst_pad_push (spdif->srcpad, buf)) != GST_FLOW_OK)
    goto push_error;

  spdif->offset += obtained;
  spdif->dataleft -= obtained;

  /* Iterate until need more data, so adapter size won't grow */
  if (spdif->streaming) {
    GST_LOG_OBJECT (spdif,
        "offset: %" G_GINT64_FORMAT " , end: %" G_GINT64_FORMAT, spdif->offset,
        spdif->end_offset);
    goto iterate_adapter;
  }
  return res;

  /* ERROR */
found_eos:
  {
    GST_DEBUG_OBJECT (spdif, "found EOS");
    return GST_FLOW_EOS;
  }
pull_error:
  {
    /* check if we got EOS */
    if (res == GST_FLOW_EOS)
      goto found_eos;

    GST_WARNING_OBJECT (spdif,
        "Error getting %d bytes from the "
        "sinkpad (dataleft = %" G_GINT64_FORMAT ")", framesize,
        spdif->dataleft);
    return res;
  }
push_error:
  {
    GST_INFO_OBJECT (spdif,
        "Error pushing on srcpad %s:%s, reason %s, is linked? = %d",
        GST_DEBUG_PAD_NAME (spdif->srcpad), gst_flow_get_name (res),
        gst_pad_is_linked (spdif->srcpad));
    return res;
  }
}

static void
gst_spdifdemux_loop (GstPad * pad)
{
  GstFlowReturn ret;
  GstSpdifDemux *spdif = GST_SPDIFDEMUX (GST_PAD_PARENT (pad));

  GST_LOG_OBJECT (spdif, "process data");

  switch (spdif->state) {
    case GST_SPDIFDEMUX_HEADER:
      GST_INFO_OBJECT (spdif, "GST_SPDIFDEMUX_HEADER");
      if ((ret = gst_spdifdemux_stream_headers (spdif)) != GST_FLOW_OK)
        goto pause;

      if (!spdif->got_fmt)
        break;

      spdif->state = GST_SPDIFDEMUX_DATA;
      GST_INFO_OBJECT (spdif, "GST_SPDIFDEMUX_DATA");

    case GST_SPDIFDEMUX_DATA:
      if ((ret = gst_spdifdemux_stream_data (spdif, FALSE)) != GST_FLOW_OK)
        goto pause;
      break;
    default:
      g_assert_not_reached ();
  }
  return;

  /* ERRORS */
pause:
  {
    const gchar *reason = gst_flow_get_name (ret);

    GST_DEBUG_OBJECT (spdif, "pausing task, reason %s", reason);
    gst_pad_pause_task (pad);

    if (ret == GST_FLOW_EOS) {
      /* handle end-of-stream/segment */
      /* so align our position with the end of it, if there is one
       * this ensures a subsequent will arrive at correct base/acc time */
      if (spdif->segment.format == GST_FORMAT_TIME) {
        if (spdif->segment.rate > 0.0 &&
            GST_CLOCK_TIME_IS_VALID (spdif->segment.stop))
          spdif->segment.position = spdif->segment.stop;
        else if (spdif->segment.rate < 0.0)
          spdif->segment.position = spdif->segment.start;
      }
      if (spdif->state == GST_SPDIFDEMUX_HEADER || !spdif->caps) {
        GST_ELEMENT_ERROR (spdif, STREAM, WRONG_TYPE, (NULL),
            ("No valid input found before end of stream"));
        gst_pad_push_event (spdif->srcpad, gst_event_new_eos ());
      } else {
        /* add pad before we perform EOS */
        if (G_UNLIKELY (spdif->first)) {
          spdif->first = FALSE;
          gst_spdifdemux_add_src_pad (spdif, NULL);
        }

        /* perform EOS logic */
        if (spdif->segment.flags & GST_SEEK_FLAG_SEGMENT) {
          GstClockTime stop;

          if ((stop = spdif->segment.stop) == -1)
            stop = spdif->segment.duration;

          gst_element_post_message (GST_ELEMENT_CAST (spdif),
              gst_message_new_segment_done (GST_OBJECT_CAST (spdif),
                  spdif->segment.format, stop));
          gst_pad_push_event (spdif->srcpad,
              gst_event_new_segment_done (spdif->segment.format, stop));
        } else {
          gst_pad_push_event (spdif->srcpad, gst_event_new_eos ());
        }
      }
    } else if (ret == GST_FLOW_NOT_LINKED || ret < GST_FLOW_EOS) {
      /* for fatal errors we post an error message, post the error
       * first so the app knows about the error first. */
      GST_ELEMENT_FLOW_ERROR (spdif, ret);
      gst_pad_push_event (spdif->srcpad, gst_event_new_eos ());
    }
    return;
  }
}

static GstFlowReturn
gst_spdifdemux_chain (GstPad * pad, GstObject * parent, GstBuffer * buf)
{
  GstFlowReturn ret;
  GstSpdifDemux *spdif = GST_SPDIFDEMUX (parent);

  GST_LOG_OBJECT (spdif, "adapter_push %" G_GSIZE_FORMAT " bytes",
      gst_buffer_get_size (buf));

  gst_adapter_push (spdif->adapter, buf);

  switch (spdif->state) {
    case GST_SPDIFDEMUX_HEADER:
      GST_INFO_OBJECT (spdif, "GST_SPDIFDEMUX_HEADER");
      if ((ret = gst_spdifdemux_stream_headers (spdif)) != GST_FLOW_OK)
        goto done;

      if (!spdif->got_fmt)
        break;

      spdif->state = GST_SPDIFDEMUX_DATA;
      GST_INFO_OBJECT (spdif, "GST_SPDIFDEMUX_DATA");

      /* fall-through */
    case GST_SPDIFDEMUX_DATA:
      if (buf && GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DISCONT))
        spdif->discont = TRUE;
      if ((ret = gst_spdifdemux_stream_data (spdif, FALSE)) != GST_FLOW_OK)
        goto done;
      break;
    default:
      g_return_val_if_reached (GST_FLOW_ERROR);
  }
done:
  if (G_UNLIKELY (spdif->abort_buffering)) {
    spdif->abort_buffering = FALSE;
    ret = GST_FLOW_ERROR;
    /* sort of demux/parse error */
    GST_ELEMENT_ERROR (spdif, STREAM, DEMUX, (NULL), ("unhandled buffer size"));
  }

  return ret;
}

static GstFlowReturn
gst_spdifdemux_flush_data (GstSpdifDemux * spdif)
{
  GstFlowReturn ret = GST_FLOW_OK;
  guint av;

  if ((av = gst_adapter_available (spdif->adapter)) > 0) {
    ret = gst_spdifdemux_stream_data (spdif, TRUE);
  }

  return ret;
}

static gboolean
gst_spdifdemux_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstSpdifDemux *spdif = GST_SPDIFDEMUX (parent);
  gboolean ret = TRUE;

  GST_LOG_OBJECT (spdif, "handling %s event", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      //if (!gst_caps_is_fixed (caps))
      //goto done;

      GST_DEBUG_OBJECT (spdif, "sink caps %" GST_PTR_FORMAT, caps);
      spdif->spec.latency_time = GST_SECOND;
      if (!gst_audio_ring_buffer_parse_caps (&spdif->spec, caps))
        goto done;
    done:
      //  gst_caps_replace (&spec.caps, NULL);
      //  return ret;


      gst_event_unref (event);
      break;
    }
    case GST_EVENT_SEGMENT:
    {
      gint64 start, stop, offset = 0, end_offset = -1;
      GstSegment segment;

      /* some debug output */
      gst_event_copy_segment (event, &segment);
      GST_DEBUG_OBJECT (spdif, "received newsegment %" GST_SEGMENT_FORMAT,
          &segment);

      gst_segment_copy_into (&segment, &spdif->segment);

      /* also store the newsegment event for the streaming thread */
      if (spdif->start_segment)
        gst_event_unref (spdif->start_segment);
      GST_DEBUG_OBJECT (spdif, "Storing newseg %" GST_SEGMENT_FORMAT, &segment);
      spdif->start_segment = gst_event_new_segment (&segment);

      /* stream leftover data in current segment */
      gst_spdifdemux_flush_data (spdif);
      /* and set up streaming thread for next one */
      spdif->offset = offset;
      spdif->end_offset = end_offset;

      if (spdif->datasize > 0 && (spdif->end_offset == -1
              || spdif->end_offset > spdif->datastart + spdif->datasize))
        spdif->end_offset = spdif->datastart + spdif->datasize;

      if (spdif->end_offset != -1) {
        spdif->dataleft = spdif->end_offset - spdif->offset;
      } else {
        /* infinity; upstream will EOS when done */
        spdif->dataleft = G_MAXUINT64;
      }
    exit:
      gst_event_unref (event);
      break;
    }
    case GST_EVENT_EOS:
      if (spdif->state == GST_SPDIFDEMUX_HEADER || !spdif->caps) {
        GST_ELEMENT_ERROR (spdif, STREAM, WRONG_TYPE, (NULL),
            ("No valid input found before end of stream"));
      } else {
        /* add pad if needed so EOS is seen downstream */
        if (G_UNLIKELY (spdif->first)) {
          spdif->first = FALSE;
          gst_spdifdemux_add_src_pad (spdif, NULL);
        }

        /* stream leftover data in current segment */
        gst_spdifdemux_flush_data (spdif);
      }

      /* fall-through */
    case GST_EVENT_FLUSH_STOP:
    {
      GstClockTime dur;

      if (spdif->adapter)
        gst_adapter_clear (spdif->adapter);
      spdif->discont = TRUE;
      dur = spdif->segment.duration;
      gst_segment_init (&spdif->segment, spdif->segment.format);
      spdif->segment.duration = dur;
      /* fall-through */
    }
    default:
      ret = gst_pad_event_default (spdif->sinkpad, parent, event);
      break;
  }

  return ret;
}

/* handle queries for location and length in requested format */
static gboolean
gst_spdifdemux_pad_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  gboolean res = TRUE;
  GstSpdifDemux *spdif = GST_SPDIFDEMUX (parent);

  /* only if we know */
  if (spdif->state != GST_SPDIFDEMUX_DATA) {
    return FALSE;
  }

  GST_LOG_OBJECT (pad, "%s query", GST_QUERY_TYPE_NAME (query));

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_SEGMENT:
    {
      GstFormat format;
      gint64 start, stop;

      format = spdif->segment.format;

      start =
          gst_segment_to_stream_time (&spdif->segment, format,
          spdif->segment.start);
      if ((stop = spdif->segment.stop) == -1)
        stop = spdif->segment.duration;
      else
        stop = gst_segment_to_stream_time (&spdif->segment, format, stop);

      gst_query_set_segment (query, spdif->segment.rate, format, start, stop);
      res = TRUE;
      break;
    }
    default:
      res = gst_pad_query_default (pad, parent, query);
      break;
  }
  return res;
}

static gboolean
gst_spdifdemux_srcpad_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstSpdifDemux *spdifdemux = GST_SPDIFDEMUX (parent);
  gboolean res = FALSE;

  GST_DEBUG_OBJECT (spdifdemux, "%s event", GST_EVENT_TYPE_NAME (event));

  switch (GST_EVENT_TYPE (event)) {
    default:
      res = gst_pad_push_event (spdifdemux->sinkpad, event);
      break;
  }
  return res;
}

static gboolean
gst_spdifdemux_sink_activate (GstPad * sinkpad, GstObject * parent)
{
  GstSpdifDemux *spdif = GST_SPDIFDEMUX (parent);
  GstQuery *query;
  gboolean pull_mode;

  if (spdif->adapter) {
    gst_adapter_clear (spdif->adapter);
    g_object_unref (spdif->adapter);
    spdif->adapter = NULL;
  }

  query = gst_query_new_scheduling ();

  if (!gst_pad_peer_query (sinkpad, query)) {
    gst_query_unref (query);
    goto activate_push;
  }

  pull_mode = gst_query_has_scheduling_mode_with_flags (query,
      GST_PAD_MODE_PULL, GST_SCHEDULING_FLAG_SEEKABLE);
  gst_query_unref (query);

  if (!pull_mode)
    goto activate_push;

  GST_DEBUG_OBJECT (sinkpad, "activating pull");
  spdif->streaming = FALSE;
  return gst_pad_activate_mode (sinkpad, GST_PAD_MODE_PULL, TRUE);

activate_push:
  {
    GST_DEBUG_OBJECT (sinkpad, "activating push");
    spdif->streaming = TRUE;
    spdif->adapter = gst_adapter_new ();
    return gst_pad_activate_mode (sinkpad, GST_PAD_MODE_PUSH, TRUE);
  }
}


static gboolean
gst_spdifdemux_sink_activate_mode (GstPad * sinkpad, GstObject * parent,
    GstPadMode mode, gboolean active)
{
  gboolean res;

  switch (mode) {
    case GST_PAD_MODE_PUSH:
      res = TRUE;
      break;
    case GST_PAD_MODE_PULL:
      if (active) {
        /* if we have a scheduler we can start the task */
        res =
            gst_pad_start_task (sinkpad, (GstTaskFunction) gst_spdifdemux_loop,
            sinkpad, NULL);
      } else {
        res = gst_pad_stop_task (sinkpad);
      }
      break;
    default:
      res = FALSE;
      break;
  }
  return res;
}

static GstStateChangeReturn
gst_spdifdemux_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstSpdifDemux *spdif = GST_SPDIFDEMUX (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      gst_spdifdemux_reset (spdif);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_spdifdemux_reset (spdif);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }
  return ret;
}

static void
gst_spdifdemux_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstSpdifDemux *self;

  g_return_if_fail (GST_IS_SPDIFDEMUX (object));
  self = GST_SPDIFDEMUX (object);

  switch (prop_id) {
    case PROP_IGNORE_LENGTH:
      self->ignore_length = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }

}

static void
gst_spdifdemux_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstSpdifDemux *self;

  g_return_if_fail (GST_IS_SPDIFDEMUX (object));
  self = GST_SPDIFDEMUX (object);

  switch (prop_id) {
    case PROP_IGNORE_LENGTH:
      g_value_set_boolean (value, self->ignore_length);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (self, prop_id, pspec);
  }
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "spdifdemux", GST_RANK_PRIMARY,
      GST_TYPE_SPDIFDEMUX);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    spdifdemux,
    "Parse a iec937 file into compressed audio",
    plugin_init, VERSION, GST_LICENSE, GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
