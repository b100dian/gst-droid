/*
 * gst-droid
 *
 * Copyright (C) 2026 Jolla Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/*
 * droidscreencapsrc records the display with the hardware H.264 encoder.
 *
 * The compositor (qt5-qpa-hwcomposer-plugin with QPA_HWC_SCREENCAP=1) renders
 * each new frame into the encoder's input Surface, so nothing is encoded while
 * the screen is idle. Output is one access unit per buffer, byte-stream, with
 * SPS/PPS on every IDR. PTS start at 0 with the first access unit and keep the
 * idle gaps. width and height are required: the display size, or its
 * transpose for a landscape recording.
 *
 *   gst-launch-1.0 -e droidscreencapsrc width=1080 height=2520 ! \
 *       h264parse ! video/x-h264,stream-format=avc,alignment=au ! \
 *       qtmux ! filesink location=/tmp/screen.mp4
 *   gst-launch-1.0 droidscreencapsrc width=1080 height=2520 ! \
 *       tcpserversink host=0.0.0.0 port=5000 sync=false
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstdroidscreencapsrc.h"
#include <errno.h>
#include <string.h>

GST_DEBUG_CATEGORY_EXTERN (gst_droid_screencapsrc_debug);
#define GST_CAT_DEFAULT gst_droid_screencapsrc_debug

#define DEFAULT_WIDTH           0       /* required */
#define DEFAULT_HEIGHT          0       /* required */
#define DEFAULT_TARGET_BITRATE  8000000
#define DEFAULT_FPS             30

/* On overflow, queued access units are dropped up to the next IDR. */
#define OUTPUT_QUEUE_MAX_FRAMES 30
#define OUTPUT_QUEUE_MAX_BYTES  (8 * 1024 * 1024)

/* Bound for draining the encoder at EOS, after its 500 ms detach grace. */
#define FINISH_TIMEOUT_MS       3000

enum
{
  PROP_0,
  PROP_WIDTH,
  PROP_HEIGHT,
  PROP_TARGET_BITRATE,
  PROP_FPS,
};

/* framerate in the caps is nominal: idle periods carry no access units. */
static GstStaticPadTemplate gst_droidscreencapsrc_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264, stream-format = (string) byte-stream, "
        "alignment = (string) au"));

static void gst_droidscreencapsrc_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_droidscreencapsrc_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_droidscreencapsrc_finalize (GObject * object);

static gboolean gst_droidscreencapsrc_send_event (GstElement * element,
    GstEvent * event);

static gboolean gst_droidscreencapsrc_start (GstBaseSrc * bsrc);
static gboolean gst_droidscreencapsrc_stop (GstBaseSrc * bsrc);
static gboolean gst_droidscreencapsrc_unlock (GstBaseSrc * bsrc);
static gboolean gst_droidscreencapsrc_unlock_stop (GstBaseSrc * bsrc);
static GstCaps *gst_droidscreencapsrc_get_caps (GstBaseSrc * bsrc,
    GstCaps * filter);
static GstFlowReturn gst_droidscreencapsrc_create (GstPushSrc * psrc,
    GstBuffer ** outbuf);

static void gst_droidscreencapsrc_data_available (void *data,
    DroidMediaCodecData * encoded);
static void gst_droidscreencapsrc_error (void *data, int err);
static void gst_droidscreencapsrc_signal_eos (void *data);

#define gst_droidscreencapsrc_parent_class parent_class
G_DEFINE_TYPE (GstDroidScreenCapSrc, gst_droidscreencapsrc, GST_TYPE_PUSH_SRC);

static void
gst_droidscreencapsrc_class_init (GstDroidScreenCapSrcClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstElementClass *gstelement_class = (GstElementClass *) klass;
  GstBaseSrcClass *gstbasesrc_class = (GstBaseSrcClass *) klass;
  GstPushSrcClass *gstpushsrc_class = (GstPushSrcClass *) klass;

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_droidscreencapsrc_src_template));

  gst_element_class_set_static_metadata (gstelement_class,
      "Sailfish screen capture source",
      "Source/Video",
      "Records the display through the hardware AVC encoder input Surface",
      "Jolla Ltd.");

  gobject_class->set_property =
      GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_set_property);
  gobject_class->get_property =
      GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_get_property);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_finalize);

  gstelement_class->send_event =
      GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_send_event);

  gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_start);
  gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_stop);
  gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_unlock);
  gstbasesrc_class->unlock_stop =
      GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_unlock_stop);
  gstbasesrc_class->get_caps =
      GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_get_caps);
  gstpushsrc_class->create = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_create);

  g_object_class_install_property (gobject_class, PROP_WIDTH,
      g_param_spec_int ("width", "Width",
          "Recording width in pixels (required): the display width, or "
          "the display height for a landscape recording",
          0, G_MAXINT, DEFAULT_WIDTH,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_HEIGHT,
      g_param_spec_int ("height", "Height",
          "Recording height in pixels (required): the display height, or "
          "the display width for a landscape recording",
          0, G_MAXINT, DEFAULT_HEIGHT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_TARGET_BITRATE,
      g_param_spec_int ("target-bitrate", "Target Bitrate",
          "Target bitrate in bits per second", 1, G_MAXINT,
          DEFAULT_TARGET_BITRATE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));

  g_object_class_install_property (gobject_class, PROP_FPS,
      g_param_spec_int ("fps", "FPS",
          "Nominal maximum frame rate published to the capture plugin",
          1, 120, DEFAULT_FPS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
          GST_PARAM_MUTABLE_READY));
}

static void
gst_droidscreencapsrc_init (GstDroidScreenCapSrc * src)
{
  src->width = DEFAULT_WIDTH;
  src->height = DEFAULT_HEIGHT;
  src->target_bitrate = DEFAULT_TARGET_BITRATE;
  src->fps = DEFAULT_FPS;

  g_mutex_init (&src->output_lock);
  g_cond_init (&src->output_cond);
  src->output_queue = g_queue_new ();

  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);
  gst_base_src_set_live (GST_BASE_SRC (src), TRUE);
}

/* *_locked functions require output_lock. */
static void
gst_droidscreencapsrc_clear_queue_locked (GstDroidScreenCapSrc * src)
{
  GstBuffer *buf;

  while ((buf = g_queue_pop_head (src->output_queue)))
    gst_buffer_unref (buf);
  src->queue_bytes = 0;
}

static void
gst_droidscreencapsrc_reset_run_state_locked (GstDroidScreenCapSrc * src)
{
  gst_droidscreencapsrc_clear_queue_locked (src);
  g_free (src->config_data);
  src->config_data = NULL;
  src->config_size = 0;

  src->have_first_pts = FALSE;
  src->first_ts = 0;
  src->last_ts = 0;

  src->waiting_for_keyframe = FALSE;
  src->dropped_frames = 0;
  src->dropped_bytes = 0;
  src->overload_episodes = 0;
  src->duplicate_pts_frames = 0;

  src->eos_requested = FALSE;
  src->finish_started = FALSE;
  src->eos = FALSE;
  src->error = 0;
  src->error_posted = FALSE;
  src->frames_out = 0;
}

/* Keeps the first error; create() posts it. */
static void
gst_droidscreencapsrc_set_error_locked (GstDroidScreenCapSrc * src, gint err)
{
  if (src->error == 0) {
    src->error = err != 0 ? err : -1;
    GST_ERROR_OBJECT (src, "run terminated by error %d", src->error);
  }
  g_cond_broadcast (&src->output_cond);
}

/* Sync frames get the SPS/PPS prepended unless the encoder already did. */
static GstBuffer *
gst_droidscreencapsrc_wrap_locked (GstDroidScreenCapSrc * src,
    DroidMediaCodecData * encoded)
{
  gsize prefix = 0;
  GstBuffer *buf;

  if (encoded->sync && src->config_data != NULL &&
      !((gsize) encoded->data.size >= src->config_size &&
          memcmp (encoded->data.data, src->config_data, src->config_size) == 0))
    prefix = src->config_size;

  buf = gst_buffer_new_allocate (NULL, prefix + encoded->data.size, NULL);
  if (buf == NULL)
    return NULL;

  if (prefix)
    gst_buffer_fill (buf, 0, src->config_data, prefix);
  gst_buffer_fill (buf, prefix, encoded->data.data, encoded->data.size);

  if (!encoded->sync)
    GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);

  return buf;
}

/* Takes ownership of @buf. */
static void
gst_droidscreencapsrc_enqueue_locked (GstDroidScreenCapSrc * src,
    GstBuffer * buf)
{
  guint queued = g_queue_get_length (src->output_queue);
  gsize size = gst_buffer_get_size (buf);

  if (queued >= OUTPUT_QUEUE_MAX_FRAMES ||
      src->queue_bytes + size > OUTPUT_QUEUE_MAX_BYTES) {
    src->overload_episodes++;
    GST_WARNING_OBJECT (src,
        "output queue bound reached (%u frames, %" G_GSIZE_FORMAT
        " bytes; limits %d/%d): episode %u, dropping %u queued + 1 "
        "frames and discarding until the next keyframe",
        queued, src->queue_bytes, OUTPUT_QUEUE_MAX_FRAMES,
        OUTPUT_QUEUE_MAX_BYTES, src->overload_episodes, queued);

    src->dropped_frames += queued + 1;
    src->dropped_bytes += src->queue_bytes + size;
    gst_droidscreencapsrc_clear_queue_locked (src);
    gst_buffer_unref (buf);
    src->waiting_for_keyframe = TRUE;
    return;
  }

  g_queue_push_tail (src->output_queue, buf);
  src->queue_bytes += size;
  g_cond_signal (&src->output_cond);
}

static void
gst_droidscreencapsrc_finalize (GObject * object)
{
  GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (object);

  GST_DEBUG_OBJECT (src, "finalize");

  g_mutex_lock (&src->output_lock);
  gst_droidscreencapsrc_reset_run_state_locked (src);
  g_mutex_unlock (&src->output_lock);

  g_queue_free (src->output_queue);
  src->output_queue = NULL;

  g_mutex_clear (&src->output_lock);
  g_cond_clear (&src->output_cond);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_droidscreencapsrc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (object);

  switch (prop_id) {
    case PROP_WIDTH:
      src->width = g_value_get_int (value);
      break;
    case PROP_HEIGHT:
      src->height = g_value_get_int (value);
      break;
    case PROP_TARGET_BITRATE:
      src->target_bitrate = g_value_get_int (value);
      break;
    case PROP_FPS:
      src->fps = g_value_get_int (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_droidscreencapsrc_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (object);

  switch (prop_id) {
    case PROP_WIDTH:
      g_value_set_int (value, src->width);
      break;
    case PROP_HEIGHT:
      g_value_set_int (value, src->height);
      break;
    case PROP_TARGET_BITRATE:
      g_value_set_int (value, src->target_bitrate);
      break;
    case PROP_FPS:
      g_value_set_int (value, src->fps);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* Encoder callbacks run on the droidmedia output thread and must not block. */
static void
gst_droidscreencapsrc_data_available (void *data, DroidMediaCodecData * encoded)
{
  GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (data);
  GstClockTime pts;
  GstBuffer *buf;

  GST_LOG_OBJECT (src, "data_available: %" G_GSIZE_FORMAT " bytes, ts %"
      G_GINT64_FORMAT " sync=%d codec_config=%d",
      (gsize) encoded->data.size, encoded->ts, encoded->sync,
      encoded->codec_config);

  g_mutex_lock (&src->output_lock);

  if (!src->running || src->flushing || src->error != 0) {
    g_mutex_unlock (&src->output_lock);
    return;
  }

  if (encoded->codec_config) {
    /* Never push SPS/PPS on their own: h264parse turns a header-only buffer
     * into an access unit without PTS, which qtmux rejects. */
    g_free (src->config_data);
    src->config_size = encoded->data.size;
    src->config_data = g_malloc (src->config_size);
    memcpy (src->config_data, encoded->data.data, src->config_size);
    GST_INFO_OBJECT (src, "retained codec config (%" G_GSIZE_FORMAT
        " bytes)", (gsize) encoded->data.size);
    g_mutex_unlock (&src->output_lock);
    return;
  }

  if (!src->have_first_pts) {
    src->have_first_pts = TRUE;
    src->first_ts = encoded->ts;
    src->last_ts = encoded->ts;
    GST_INFO_OBJECT (src, "timestamp origin: codec ts %" G_GINT64_FORMAT,
        src->first_ts);
  } else if (encoded->ts < src->last_ts) {
    GST_ERROR_OBJECT (src, "regressing codec ts %" G_GINT64_FORMAT
        " after %" G_GINT64_FORMAT, encoded->ts, src->last_ts);
    gst_droidscreencapsrc_set_error_locked (src, -EINVAL);
    g_mutex_unlock (&src->output_lock);
    return;
  } else if (encoded->ts == src->last_ts) {
    /* The encoder may emit its first IDR twice. Muxers reject a
     * zero-duration sample, so keep the first one. */
    src->duplicate_pts_frames++;
    GST_WARNING_OBJECT (src, "discarding access unit repeating codec ts %"
        G_GINT64_FORMAT " (%" G_GSIZE_FORMAT " bytes, sync=%d)",
        encoded->ts, (gsize) encoded->data.size, encoded->sync);
    g_mutex_unlock (&src->output_lock);
    return;
  }

  src->last_ts = encoded->ts;
  pts = encoded->ts - src->first_ts;

  if (src->waiting_for_keyframe) {
    if (!encoded->sync) {
      src->dropped_frames++;
      src->dropped_bytes += encoded->data.size;
      g_mutex_unlock (&src->output_lock);
      return;
    }
    GST_WARNING_OBJECT (src, "overload episode %u ended: dropped %u "
        "frames / %" G_GSIZE_FORMAT " bytes; resuming at keyframe PTS %"
        GST_TIME_FORMAT, src->overload_episodes, src->dropped_frames,
        src->dropped_bytes, GST_TIME_ARGS (pts));
    src->waiting_for_keyframe = FALSE;
  }

  buf = gst_droidscreencapsrc_wrap_locked (src, encoded);
  if (buf == NULL) {
    GST_ERROR_OBJECT (src, "cannot allocate a %" G_GSIZE_FORMAT
        " byte buffer", (gsize) encoded->data.size);
    gst_droidscreencapsrc_set_error_locked (src, -ENOMEM);
    g_mutex_unlock (&src->output_lock);
    return;
  }

  /* No duration: it is only known once the next access unit arrives, and
   * waiting for that would keep the last frame of a still screen from a
   * live viewer. */
  GST_BUFFER_PTS (buf) = pts;
  gst_droidscreencapsrc_enqueue_locked (src, buf);
  g_mutex_unlock (&src->output_lock);
}

static void
gst_droidscreencapsrc_error (void *data, int err)
{
  GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (data);

  GST_ERROR_OBJECT (src, "encoder error %d (0x%x)", err, (unsigned) -err);

  g_mutex_lock (&src->output_lock);
  gst_droidscreencapsrc_set_error_locked (src, err);
  g_mutex_unlock (&src->output_lock);
}

static void
gst_droidscreencapsrc_signal_eos (void *data)
{
  GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (data);

  GST_INFO_OBJECT (src, "encoder reached EOS");

  g_mutex_lock (&src->output_lock);
  src->eos = TRUE;
  g_cond_broadcast (&src->output_cond);
  g_mutex_unlock (&src->output_lock);
}

/*
 * GstBaseSrc answers EOS by flushing and ending the stream without another
 * create(), which would lose the encoder's last access units. Intercept it;
 * create() finishes the encoder on the streaming thread. Called with the
 * STATE_LOCK held, so only set flags here.
 */
static gboolean
gst_droidscreencapsrc_send_event (GstElement * element, GstEvent * event)
{
  GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (element);

  if (GST_EVENT_TYPE (event) == GST_EVENT_EOS) {
    gboolean handled = FALSE;

    g_mutex_lock (&src->output_lock);
    if (src->running && !src->flushing) {
      if (!src->eos_requested) {
        src->eos_requested = TRUE;
        GST_INFO_OBJECT (src, "EOS requested; finishing encoder gracefully");
      }
      g_cond_broadcast (&src->output_cond);
      handled = TRUE;
    }
    g_mutex_unlock (&src->output_lock);

    if (handled) {
      gst_event_unref (event);
      return TRUE;
    }
  }

  return GST_ELEMENT_CLASS (parent_class)->send_event (element, event);
}

static GstCaps *
gst_droidscreencapsrc_get_caps (GstBaseSrc * bsrc, GstCaps * filter)
{
  GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (bsrc);
  GstCaps *caps;

  if (src->width > 0 && src->height > 0) {
    caps = gst_caps_new_simple ("video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au",
        "width", G_TYPE_INT, src->width,
        "height", G_TYPE_INT, src->height,
        "framerate", GST_TYPE_FRACTION, src->fps, 1, NULL);
  } else {
    caps = gst_pad_get_pad_template_caps (GST_BASE_SRC_PAD (bsrc));
  }

  if (filter) {
    GstCaps *tmp = gst_caps_intersect_full (filter, caps,
        GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = tmp;
  }

  GST_DEBUG_OBJECT (src, "caps %" GST_PTR_FORMAT, caps);
  return caps;
}

static gboolean
gst_droidscreencapsrc_start (GstBaseSrc * bsrc)
{
  GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (bsrc);
  DroidMediaScreenCaptureCallbacks cb;

  GST_DEBUG_OBJECT (src, "start");

  g_mutex_lock (&src->output_lock);
  gst_droidscreencapsrc_reset_run_state_locked (src);
  src->running = FALSE;
  src->flushing = FALSE;
  g_cond_broadcast (&src->output_cond);
  g_mutex_unlock (&src->output_lock);

  if (src->width <= 0 || src->height <= 0) {
    GST_ELEMENT_ERROR (src, RESOURCE, SETTINGS,
        ("The width and height properties must be set"), (NULL));
    return FALSE;
  }

  if (src->target_bitrate <= 0 || src->fps <= 0) {
    GST_ELEMENT_ERROR (src, RESOURCE, SETTINGS,
        ("Invalid encoder configuration"),
        ("target-bitrate=%d fps=%d", src->target_bitrate, src->fps));
    return FALSE;
  }

  memset (&cb, 0, sizeof (cb));
  cb.data_available = gst_droidscreencapsrc_data_available;
  cb.error = gst_droidscreencapsrc_error;
  cb.signal_eos = gst_droidscreencapsrc_signal_eos;

  src->capture = droid_media_screen_capture_new (src->width, src->height,
      src->target_bitrate, src->fps, &cb, src);
  if (src->capture == NULL) {
    GST_ELEMENT_ERROR (src, LIBRARY, INIT,
        ("Failed to create the hardware AVC Surface encoder"),
        ("%dx%d @%d fps, %d bps", src->width, src->height, src->fps,
            src->target_bitrate));
    return FALSE;
  }

  /* Callbacks may fire as soon as the codec starts; accept them. */
  g_mutex_lock (&src->output_lock);
  src->running = TRUE;
  g_mutex_unlock (&src->output_lock);

  if (!droid_media_screen_capture_start (src->capture)) {
    g_mutex_lock (&src->output_lock);
    src->running = FALSE;
    g_mutex_unlock (&src->output_lock);

    droid_media_screen_capture_destroy (src->capture);
    src->capture = NULL;

    g_mutex_lock (&src->output_lock);
    gst_droidscreencapsrc_reset_run_state_locked (src);
    g_mutex_unlock (&src->output_lock);

    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
        ("Failed to start screen capture"),
        ("could not start the encoder or register it with "
            "sailfish.screencap; see logcat"));
    return FALSE;
  }

  GST_INFO_OBJECT (src, "started: %dx%d @%d fps, %d bps, queue bound %d "
      "frames / %d bytes", src->width, src->height, src->fps,
      src->target_bitrate, OUTPUT_QUEUE_MAX_FRAMES, OUTPUT_QUEUE_MAX_BYTES);
  return TRUE;
}

static gboolean
gst_droidscreencapsrc_stop (GstBaseSrc * bsrc)
{
  GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (bsrc);

  GST_DEBUG_OBJECT (src, "stop");

  g_mutex_lock (&src->output_lock);
  src->running = FALSE;
  src->flushing = TRUE;
  g_cond_broadcast (&src->output_cond);
  if (src->waiting_for_keyframe) {
    GST_WARNING_OBJECT (src, "stopping during overload episode %u: "
        "dropped %u frames / %" G_GSIZE_FORMAT " bytes",
        src->overload_episodes, src->dropped_frames, src->dropped_bytes);
  }
  GST_INFO_OBJECT (src, "run summary: %" G_GUINT64_FORMAT " frames pushed, "
      "%u overload episodes, %u frames dropped, %u duplicate-PTS frames "
      "discarded", src->frames_out, src->overload_episodes,
      src->dropped_frames, src->duplicate_pts_frames);
  g_mutex_unlock (&src->output_lock);

  /* Not under output_lock: a callback may be waiting for it. This also
   * waits for a finish still running on the streaming thread. */
  if (src->capture) {
    droid_media_screen_capture_stop (src->capture);
    droid_media_screen_capture_destroy (src->capture);
    src->capture = NULL;
  }

  g_mutex_lock (&src->output_lock);
  gst_droidscreencapsrc_reset_run_state_locked (src);
  src->flushing = FALSE;
  g_mutex_unlock (&src->output_lock);

  return TRUE;
}

static gboolean
gst_droidscreencapsrc_unlock (GstBaseSrc * bsrc)
{
  GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (bsrc);

  GST_DEBUG_OBJECT (src, "unlock");

  g_mutex_lock (&src->output_lock);
  src->flushing = TRUE;
  g_cond_broadcast (&src->output_cond);
  g_mutex_unlock (&src->output_lock);

  return TRUE;
}

static gboolean
gst_droidscreencapsrc_unlock_stop (GstBaseSrc * bsrc)
{
  GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (bsrc);

  GST_DEBUG_OBJECT (src, "unlock_stop");

  g_mutex_lock (&src->output_lock);
  src->flushing = FALSE;
  g_mutex_unlock (&src->output_lock);

  return TRUE;
}

/* Called without output_lock. */
static void
gst_droidscreencapsrc_do_finish (GstDroidScreenCapSrc * src)
{
  gboolean ok = FALSE;

  if (src->capture) {
    GST_INFO_OBJECT (src, "finishing encoder (timeout %d ms)",
        FINISH_TIMEOUT_MS);
    ok = droid_media_screen_capture_finish (src->capture, FINISH_TIMEOUT_MS);
  }

  g_mutex_lock (&src->output_lock);
  if (!ok) {
    GST_WARNING_OBJECT (src, "encoder did not reach codec EOS; "
        "finalizing with the data received so far");
  }
  /* The output thread has stopped; nothing more can arrive. */
  src->eos = TRUE;
  g_cond_broadcast (&src->output_cond);
  g_mutex_unlock (&src->output_lock);
}

static GstFlowReturn
gst_droidscreencapsrc_create (GstPushSrc * psrc, GstBuffer ** outbuf)
{
  GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (psrc);
  GstBuffer *buf;

  g_mutex_lock (&src->output_lock);

  for (;;) {
    if (src->flushing) {
      g_mutex_unlock (&src->output_lock);
      return GST_FLOW_FLUSHING;
    }

    buf = g_queue_pop_head (src->output_queue);
    if (buf != NULL) {
      src->queue_bytes -= gst_buffer_get_size (buf);
      break;
    }

    if (src->error != 0) {
      gint err = src->error;
      gboolean post = !src->error_posted;

      src->error_posted = TRUE;
      g_mutex_unlock (&src->output_lock);
      if (post) {
        GST_ELEMENT_ERROR (src, LIBRARY, FAILED,
            ("Screen capture encoder failed"),
            ("encoder error %d (0x%x)", err, (unsigned) -err));
      }
      return GST_FLOW_ERROR;
    }

    if (src->eos) {
      g_mutex_unlock (&src->output_lock);
      GST_INFO_OBJECT (src, "EOS after %" G_GUINT64_FORMAT " frames",
          src->frames_out);
      return GST_FLOW_EOS;
    }

    if (src->eos_requested && !src->finish_started) {
      src->finish_started = TRUE;
      g_mutex_unlock (&src->output_lock);
      gst_droidscreencapsrc_do_finish (src);
      g_mutex_lock (&src->output_lock);
      continue;
    }

    g_cond_wait (&src->output_cond, &src->output_lock);
  }

  src->frames_out++;
  g_mutex_unlock (&src->output_lock);

  GST_DEBUG_OBJECT (src, "pushing %" GST_PTR_FORMAT, buf);
  *outbuf = buf;
  return GST_FLOW_OK;
}
