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
 * gstdroidscreencapsrc.c
 *
 * GStreamer source element that captures the Android display via
 * the HWC plugin's BufferQueue and encodes it to H.264 using the
 * platform's hardware video encoder in metadata-input mode.
 *
 * Pipeline (simplified):
 *
 *   HWC plugin                          GStreamer process
 *   ──────────                          ─────────────────
 *   GraphicBuffer(RGBA)                 droidscreencapsrc
 *     │                                       │
 *     ├── BufferQueue::queueBuffer            ├── ScreenCaptureMediaSource
 *     │                                       │     (metadata mode)
 *     │                                       ├── droid_media_codec_create_encoder_raw()
 *     │                                       │     → HW encoder (OMX/Codec2)
 *     │                                       ├── Poll thread → encoded H.264 NAL
 *     │                                       └── create() → push downstream
 *
 * Usage:
 *   gst-launch-1.0 droidscreencapsrc target-bitrate=8000000 \
 *       fps=30 color-format=2130708361 ! h264parse ! ...
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstdroidscreencapsrc.h"
#include "droidmedia.h"
#include "screen_capture_encoder.h"
#include <string.h>

/* OMX_COLOR_FormatAndroidOpaque — tells the encoder to read pixel data
 * directly from the GraphicBuffer handle (metadata mode). */
#define OMX_COLOR_FormatAndroidOpaque  0x7F000789

GST_DEBUG_CATEGORY_EXTERN (gst_droid_screencapsrc_debug);
#define GST_CAT_DEFAULT gst_droid_screencapsrc_debug

/* Defaults */
#define DEFAULT_TARGET_BITRATE  8000000
#define DEFAULT_FPS             30
#define DEFAULT_COLOR_FORMAT    OMX_COLOR_FormatAndroidOpaque

/* Retry parameters for consumer lookup */
#define CONSUMER_RETRY_MS       100
#define CONSUMER_MAX_RETRIES    50   /* 5 seconds total */

enum
{
    PROP_0,
    PROP_TARGET_BITRATE,
    PROP_FPS,
    PROP_COLOR_FORMAT,
};

/* Pad template — raw H.264 byte-stream output */
static GstStaticPadTemplate gst_droidscreencapsrc_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264, "
        "stream-format = (string) byte-stream, "
        "alignment = (string) au, "
        "profile = (string) { baseline, main, high }"));

/* ----------------------------------------------------------------
 * Prototypes
 * ---------------------------------------------------------------- */
static void gst_droidscreencapsrc_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_droidscreencapsrc_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_droidscreencapsrc_finalize (GObject * object);

static gboolean gst_droidscreencapsrc_start (GstBaseSrc * bsrc);
static gboolean gst_droidscreencapsrc_stop (GstBaseSrc * bsrc);
static GstFlowReturn gst_droidscreencapsrc_create (GstPushSrc * psrc,
    GstBuffer ** outbuf);

/* Encoder callbacks — called from the encoder poll thread */
static void gst_droidscreencapsrc_data_available (void *user,
    const ScreenCaptureEncodedFrame *frame);
static void gst_droidscreencapsrc_error (void *user, int err);
static void gst_droidscreencapsrc_eos (void *user);

#define gst_droidscreencapsrc_parent_class parent_class
G_DEFINE_TYPE (GstDroidScreenCapSrc, gst_droidscreencapsrc, GST_TYPE_PUSH_SRC);

/* ----------------------------------------------------------------
 * GObject boilerplate
 * ---------------------------------------------------------------- */
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
        "Android screen capture source",
        "Source/Video",
        "Captures the Android display via HWC BufferQueue and encodes to H.264",
        "Jolla Ltd.");

    gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_set_property);
    gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_get_property);
    gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_finalize);

    gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_start);
    gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_stop);
    gstpushsrc_class->create = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_create);

    g_object_class_install_property (gobject_class, PROP_TARGET_BITRATE,
        g_param_spec_int ("target-bitrate", "Target Bitrate",
            "Target bitrate in bits per second", 0, G_MAXINT,
            DEFAULT_TARGET_BITRATE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property (gobject_class, PROP_FPS,
        g_param_spec_int ("fps", "FPS",
            "Target frame rate", 1, 120,
            DEFAULT_FPS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property (gobject_class, PROP_COLOR_FORMAT,
        g_param_spec_int ("color-format", "Color Format",
            "OMX color format for encoder input", 0, G_MAXINT32,
            DEFAULT_COLOR_FORMAT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_droidscreencapsrc_init (GstDroidScreenCapSrc * src)
{
    src->queue = NULL;
    src->encoder = NULL;
    src->target_bitrate = DEFAULT_TARGET_BITRATE;
    src->fps = DEFAULT_FPS;
    src->color_format = DEFAULT_COLOR_FORMAT;

    g_mutex_init (&src->output_lock);
    g_cond_init (&src->output_cond);
    src->output_queue = g_queue_new ();

    src->eos = FALSE;
    src->running = FALSE;

    gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);
    gst_base_src_set_live (GST_BASE_SRC (src), TRUE);
}

static void
gst_droidscreencapsrc_finalize (GObject * object)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (object);

    GST_DEBUG_OBJECT (src, "finalize");

    g_mutex_clear (&src->output_lock);
    g_cond_clear (&src->output_cond);

    if (src->output_queue) {
        g_queue_free_full (src->output_queue, g_free);
        src->output_queue = NULL;
    }

    G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_droidscreencapsrc_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (object);

    switch (prop_id) {
        case PROP_TARGET_BITRATE:
            src->target_bitrate = g_value_get_int (value);
            break;
        case PROP_FPS:
            src->fps = g_value_get_int (value);
            break;
        case PROP_COLOR_FORMAT:
            src->color_format = g_value_get_int (value);
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
        case PROP_TARGET_BITRATE:
            g_value_set_int (value, src->target_bitrate);
            break;
        case PROP_FPS:
            g_value_set_int (value, src->fps);
            break;
        case PROP_COLOR_FORMAT:
            g_value_set_int (value, src->color_format);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

/* ----------------------------------------------------------------
 * Encoder callbacks
 * ---------------------------------------------------------------- */

typedef struct {
    guint8 *data;
    gsize size;
    gint64 timestamp_ns;
    gboolean sync;
    gboolean codec_config;
} EncodedFrame;

static void
gst_droidscreencapsrc_data_available (void *user,
    const ScreenCaptureEncodedFrame *frame)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (user);
    EncodedFrame *ef;

    GST_DEBUG_OBJECT (src, "data_available: %zd bytes, ts=%" G_GINT64_FORMAT
        " sync=%d codec_config=%d", frame->size, frame->timestamp_ns,
        frame->sync, frame->codec_config);

    ef = g_new (EncodedFrame, 1);
    ef->data = g_memdup2 (frame->data, frame->size);
    ef->size = frame->size;
    ef->timestamp_ns = frame->timestamp_ns;
    ef->sync = frame->sync;
    ef->codec_config = frame->codec_config;

    g_mutex_lock (&src->output_lock);
    g_queue_push_tail (src->output_queue, ef);
    g_cond_signal (&src->output_cond);
    g_mutex_unlock (&src->output_lock);
}

static void
gst_droidscreencapsrc_error (void *user, int err)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (user);

    GST_ELEMENT_ERROR (src, LIBRARY, FAILED, (NULL),
        ("Encoder error (0x%x)", -err));
}

static void
gst_droidscreencapsrc_eos (void *user)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (user);

    GST_DEBUG_OBJECT (src, "encoder signaled EOS");

    g_mutex_lock (&src->output_lock);
    src->eos = TRUE;
    g_cond_signal (&src->output_cond);
    g_mutex_unlock (&src->output_lock);
}

/* ----------------------------------------------------------------
 * GstBaseSrc vmethods
 * ---------------------------------------------------------------- */

static gboolean
gst_droidscreencapsrc_start (GstBaseSrc * bsrc)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (bsrc);
    ScreenCaptureEncoderCallbacks cb;
    int retries = 0;

    GST_DEBUG_OBJECT (src, "start");

    src->eos = FALSE;

    /* 1. Fetch consumer via Binder — retry with backoff */
    while (src->queue == NULL && retries < CONSUMER_MAX_RETRIES) {
        src->queue = droid_media_screen_capture_consumer_new ();
        if (src->queue == NULL) {
            if (retries == 0) {
                GST_WARNING_OBJECT (src,
                    "sailfish.screencap service not ready — retrying");
            }
            g_usleep (CONSUMER_RETRY_MS * 1000);
            retries++;
        }
    }

    if (src->queue == NULL) {
        GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
            ("Failed to connect to screen capture service"),
            ("sailfish.screencap not available after %d retries", retries));
        return FALSE;
    }

    GST_INFO_OBJECT (src, "capture consumer obtained (after %d retries)", retries);

    /* 2. Build encoder pipeline */
    memset (&cb, 0, sizeof (cb));
    cb.data_available = gst_droidscreencapsrc_data_available;
    cb.error = gst_droidscreencapsrc_error;
    cb.eos = gst_droidscreencapsrc_eos;

    src->encoder = screen_capture_encoder_new (
        src->queue,
        0,  /* width  — auto-detected from consumer */
        0,  /* height — auto-detected from consumer */
        src->color_format,
        src->target_bitrate,
        src->fps,
        &cb,
        src);

    if (src->encoder == NULL) {
        GST_ELEMENT_ERROR (src, LIBRARY, INIT, (NULL),
            ("Failed to create screen capture encoder"));
        return FALSE;
    }

    /* 3. Start the encoder + poll thread */
    if (!screen_capture_encoder_start (src->encoder)) {
        GST_ELEMENT_ERROR (src, LIBRARY, INIT, (NULL),
            ("Failed to start screen capture encoder"));
        screen_capture_encoder_destroy (src->encoder);
        src->encoder = NULL;
        return FALSE;
    }

    src->running = TRUE;
    GST_INFO_OBJECT (src, "started successfully");

    return TRUE;
}

static gboolean
gst_droidscreencapsrc_stop (GstBaseSrc * bsrc)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (bsrc);

    GST_DEBUG_OBJECT (src, "stop");

    src->running = FALSE;

    /* Stop encoder + join poll thread */
    if (src->encoder) {
        screen_capture_encoder_stop (src->encoder);
        screen_capture_encoder_destroy (src->encoder);
        src->encoder = NULL;
    }

    src->queue = NULL;

    /* Drain any remaining frames in the output queue */
    g_mutex_lock (&src->output_lock);
    while (!g_queue_is_empty (src->output_queue)) {
        EncodedFrame *ef = (EncodedFrame *) g_queue_pop_head (src->output_queue);
        g_free (ef->data);
        g_free (ef);
    }
    src->eos = FALSE;
    g_mutex_unlock (&src->output_lock);

    return TRUE;
}

/* ----------------------------------------------------------------
 * GstPushSrc create() — pop from output queue, push downstream
 * ---------------------------------------------------------------- */
static GstFlowReturn
gst_droidscreencapsrc_create (GstPushSrc * psrc, GstBuffer ** outbuf)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (psrc);
    EncodedFrame *ef;
    GstBuffer *buf;
    GstFlowReturn ret = GST_FLOW_OK;

    g_mutex_lock (&src->output_lock);

    /* Wait until a frame is available or EOS */
    while (g_queue_is_empty (src->output_queue) && !src->eos) {
        g_cond_wait (&src->output_cond, &src->output_lock);
    }

    if (src->eos && g_queue_is_empty (src->output_queue)) {
        g_mutex_unlock (&src->output_lock);
        GST_DEBUG_OBJECT (src, "EOS");
        return GST_FLOW_EOS;
    }

    ef = (EncodedFrame *) g_queue_pop_head (src->output_queue);
    g_mutex_unlock (&src->output_lock);

    /* Build GstBuffer */
    buf = gst_buffer_new_allocate (NULL, ef->size, NULL);
    if (!buf) {
        g_free (ef->data);
        g_free (ef);
        return GST_FLOW_ERROR;
    }

    {
        GstMapInfo info;
        if (gst_buffer_map (buf, &info, GST_MAP_WRITE)) {
            memcpy (info.data, ef->data, ef->size);
            gst_buffer_unmap (buf, &info);
        } else {
            gst_buffer_unref (buf);
            g_free (ef->data);
            g_free (ef);
            return GST_FLOW_ERROR;
        }
    }

    GST_BUFFER_PTS (buf) = ef->timestamp_ns;
    GST_BUFFER_DTS (buf) = GST_CLOCK_TIME_NONE;

    if (ef->sync) {
        /* DELTA_UNIT unset = keyframe (sync point) */
        GST_BUFFER_FLAG_UNSET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
    } else {
        GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
    }

    /* Codec config (SPS/PPS) */
    if (ef->codec_config) {
        GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_HEADER);
    }

    GST_DEBUG_OBJECT (src,
        "pushing buffer: %" G_GSIZE_FORMAT " bytes, PTS=%" GST_TIME_FORMAT
        " sync=%d codec_config=%d",
        ef->size, GST_TIME_ARGS (ef->timestamp_ns),
        ef->sync, ef->codec_config);

    g_free (ef->data);
    g_free (ef);

    *outbuf = buf;
    return ret;
}