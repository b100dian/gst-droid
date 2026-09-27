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
 * GStreamer source element that records the Sailfish display as H.264 using
 * the hardware AVC encoder's input Surface. The QPA hwcomposer plugin blits
 * each presented frame into that Surface; MediaCodec output is copied into
 * GstBuffers here.
 *
 *   QPA/HWC RGBA GraphicBuffer
 *     -> one GPU EGLImage texture blit (in lipstick)
 *     -> MediaCodec input Surface (published via sailfish.screencap)
 *     -> hardware AVC encoder
 *     -> ScreenCaptureSurfaceEncoder drain thread
 *     -> data_available() copies the access unit
 *     -> create() pushes video/x-h264,stream-format=byte-stream,alignment=au
 *
 * Timestamps: MediaCodec returns the presentation timestamp QPA submitted
 * (absolute CLOCK_MONOTONIC microseconds). The first ordinary access unit
 * defines the origin; every buffer PTS is (pts_us - first_pts_us) * 1000.
 * Capture is event-driven, so idle periods appear as PTS gaps, not as
 * duplicate frames. To give each sample a duration, the newest ordinary
 * access unit is held back until its successor arrives (one-frame latency);
 * the last one is finalized to the recording stop time on EOS.
 *
 * Codec configuration: MediaCodec delivers SPS/PPS as a separate
 * BUFFER_FLAG_CODECCONFIG output whose timestamp is not a presentation time.
 * It is never pushed as its own buffer: with alignment=au a header-only
 * buffer would be a second "access unit" with a PTS colliding with the first
 * IDR, and the h264parse -> AVC conversion needed by qtmux/mp4mux turns it
 * into a slice-less frame with no PTS, which qtmux rejects. Instead the
 * parameter sets are retained and prepended in-band to every sync frame
 * that does not already start with them, so each IDR is self-describing.
 *
 * EOS: GstBaseSrc (push mode) stops calling create() as soon as it receives
 * a downstream EOS through send_event(). This element therefore intercepts
 * EOS in its own send_event(), and the streaming thread performs a graceful
 * encoder finish (unregister producer, signalEndOfInputStream, drain to
 * codec EOS) before returning GST_FLOW_EOS.
 *
 * Usage:
 *   gst-launch-1.0 -e droidscreencapsrc width=1080 height=2520 \
 *       target-bitrate=8000000 fps=30 ! h264parse ! matroskamux ! \
 *       filesink location=/tmp/screen.mkv
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstdroidscreencapsrc.h"
#include "droidmedia.h"
#include "screen_capture_surface_encoder.h"
#include <errno.h>
#include <string.h>

GST_DEBUG_CATEGORY_EXTERN (gst_droid_screencapsrc_debug);
#define GST_CAT_DEFAULT gst_droid_screencapsrc_debug

/* Defaults */
#define DEFAULT_WIDTH           0       /* 0: discover from sailfish.screencap */
#define DEFAULT_HEIGHT          0
#define DEFAULT_TARGET_BITRATE  8000000
#define DEFAULT_FPS             30

/* Bounded output queue: ordinary access units only. Codec configuration is
 * retained separately and never counts against these limits. */
#define OUTPUT_QUEUE_MAX_FRAMES 30
#define OUTPUT_QUEUE_MAX_BYTES  (8 * 1024 * 1024)

/* Upper bound for draining the codec to EOS on graceful finish. The encoder
 * adds its own bounded QPA detach grace interval before this. */
#define FINISH_TIMEOUT_MS       3000

enum
{
    PROP_0,
    PROP_WIDTH,
    PROP_HEIGHT,
    PROP_TARGET_BITRATE,
    PROP_FPS,
};

/* Pad template. No profile is advertised; h264parse derives it from the
 * stream. framerate is the nominal encoder rate, not a promise that every
 * period carries an access unit. */
static GstStaticPadTemplate gst_droidscreencapsrc_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h264, "
        "stream-format = (string) byte-stream, "
        "alignment = (string) au"));

struct _GstDroidScreenCapFrame {
    guint8 *data;
    gsize size;
    GstClockTime pts;
    GstClockTime duration;
    gboolean sync;
};

/* ----------------------------------------------------------------
 * Prototypes
 * ---------------------------------------------------------------- */
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

/* Encoder callbacks — called from the encoder drain thread */
static void gst_droidscreencapsrc_data_available (void *user,
    const ScreenCaptureSurfaceEncodedFrame *frame);
static void gst_droidscreencapsrc_format_changed (void *user, int width,
    int height);
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
        "Sailfish screen capture source",
        "Source/Video",
        "Records the display through the hardware AVC encoder input Surface",
        "Jolla Ltd.");

    gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_set_property);
    gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_get_property);
    gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_finalize);

    gstelement_class->send_event = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_send_event);

    gstbasesrc_class->start = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_start);
    gstbasesrc_class->stop = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_stop);
    gstbasesrc_class->unlock = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_unlock);
    gstbasesrc_class->unlock_stop = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_unlock_stop);
    gstbasesrc_class->get_caps = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_get_caps);
    gstpushsrc_class->create = GST_DEBUG_FUNCPTR (gst_droidscreencapsrc_create);

    g_object_class_install_property (gobject_class, PROP_WIDTH,
        g_param_spec_int ("width", "Width",
            "Capture width in pixels (0: query sailfish.screencap)",
            0, G_MAXINT, DEFAULT_WIDTH,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property (gobject_class, PROP_HEIGHT,
        g_param_spec_int ("height", "Height",
            "Capture height in pixels (0: query sailfish.screencap)",
            0, G_MAXINT, DEFAULT_HEIGHT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property (gobject_class, PROP_TARGET_BITRATE,
        g_param_spec_int ("target-bitrate", "Target Bitrate",
            "Target bitrate in bits per second", 1, G_MAXINT,
            DEFAULT_TARGET_BITRATE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property (gobject_class, PROP_FPS,
        g_param_spec_int ("fps", "FPS",
            "Nominal maximum frame rate published to the capture plugin",
            1, 120, DEFAULT_FPS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_droidscreencapsrc_init (GstDroidScreenCapSrc * src)
{
    src->width = DEFAULT_WIDTH;
    src->height = DEFAULT_HEIGHT;
    src->target_bitrate = DEFAULT_TARGET_BITRATE;
    src->fps = DEFAULT_FPS;
    src->enc_width = 0;
    src->enc_height = 0;
    src->encoder = NULL;

    g_mutex_init (&src->output_lock);
    g_cond_init (&src->output_cond);
    src->output_queue = g_queue_new ();
    src->queue_bytes = 0;
    src->pending = NULL;
    src->config_data = NULL;
    src->config_size = 0;

    src->have_first_pts = FALSE;
    src->first_pts_us = 0;
    src->last_pts_us = 0;

    src->waiting_for_keyframe = FALSE;
    src->dropped_frames = 0;
    src->dropped_bytes = 0;
    src->overload_episodes = 0;
    src->duplicate_pts_frames = 0;

    src->running = FALSE;
    src->flushing = FALSE;
    src->eos_requested = FALSE;
    src->finish_started = FALSE;
    src->eos = FALSE;
    src->stop_time_us = 0;
    src->error = 0;
    src->error_posted = FALSE;
    src->frames_out = 0;

    gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);
    gst_base_src_set_live (GST_BASE_SRC (src), TRUE);
}

/* ----------------------------------------------------------------
 * Frame helpers. All *_locked helpers require output_lock.
 * ---------------------------------------------------------------- */

static void
gst_droidscreencapsrc_free_frame (GstDroidScreenCapFrame * f)
{
    if (f) {
        g_free (f->data);
        g_free (f);
    }
}

static void
gst_droidscreencapsrc_clear_queue_locked (GstDroidScreenCapSrc * src)
{
    while (!g_queue_is_empty (src->output_queue)) {
        gst_droidscreencapsrc_free_frame (
            (GstDroidScreenCapFrame *) g_queue_pop_head (src->output_queue));
    }
    src->queue_bytes = 0;
}


/* Reset every per-run field. Requires output_lock. */
static void
gst_droidscreencapsrc_reset_run_state_locked (GstDroidScreenCapSrc * src)
{
    gst_droidscreencapsrc_clear_queue_locked (src);
    gst_droidscreencapsrc_free_frame (src->pending);
    src->pending = NULL;
    g_free (src->config_data);
    src->config_data = NULL;
    src->config_size = 0;

    src->have_first_pts = FALSE;
    src->first_pts_us = 0;
    src->last_pts_us = 0;

    src->waiting_for_keyframe = FALSE;
    src->dropped_frames = 0;
    src->dropped_bytes = 0;
    src->overload_episodes = 0;
    src->duplicate_pts_frames = 0;

    src->eos_requested = FALSE;
    src->finish_started = FALSE;
    src->eos = FALSE;
    src->stop_time_us = 0;
    src->error = 0;
    src->error_posted = FALSE;
    src->frames_out = 0;
}

/* Record the first error and wake create() so it can report it. */
static void
gst_droidscreencapsrc_set_error_locked (GstDroidScreenCapSrc * src, gint err)
{
    if (src->error == 0) {
        src->error = err != 0 ? err : -1;
        GST_ERROR_OBJECT (src, "run terminated by error %d", src->error);
    }
    g_cond_broadcast (&src->output_cond);
}

/*
 * Copy an encoded sync frame, prepending the retained SPS/PPS unless the
 * access unit already begins with them (the encoder is configured with
 * prepend-sps-pps-to-idr-frames, but that is a request, not a guarantee).
 * Requires output_lock for config_data.
 */
static guint8 *
gst_droidscreencapsrc_copy_sync_frame_locked (GstDroidScreenCapSrc * src,
    const guint8 * data, gsize size, gsize * out_size)
{
    guint8 *buf;

    if (src->config_data == NULL || src->config_size == 0 ||
        (size >= src->config_size &&
         memcmp (data, src->config_data, src->config_size) == 0)) {
        *out_size = size;
        return g_memdup2 (data, size);
    }

    buf = g_malloc (src->config_size + size);
    memcpy (buf, src->config_data, src->config_size);
    memcpy (buf + src->config_size, data, size);
    *out_size = src->config_size + size;
    GST_DEBUG_OBJECT (src, "prepended %" G_GSIZE_FORMAT " bytes of codec "
        "config to a sync frame", src->config_size);
    return buf;
}

/*
 * Enqueue a finalized ordinary frame, applying the overload policy:
 * on reaching a bound, drop everything queued plus this frame, and discard
 * incoming frames until the next sync frame. Takes ownership of @f.
 */
static void
gst_droidscreencapsrc_enqueue_ordinary_locked (GstDroidScreenCapSrc * src,
    GstDroidScreenCapFrame * f)
{
    guint queued = g_queue_get_length (src->output_queue);

    if (queued >= OUTPUT_QUEUE_MAX_FRAMES ||
        src->queue_bytes + f->size > OUTPUT_QUEUE_MAX_BYTES) {
        gsize dropped_bytes = src->queue_bytes + f->size;

        src->overload_episodes++;
        GST_WARNING_OBJECT (src,
            "output queue bound reached (%u frames, %" G_GSIZE_FORMAT
            " bytes; limits %d/%d): episode %u, dropping %u queued + 1 "
            "frames and discarding until the next keyframe",
            queued, src->queue_bytes, OUTPUT_QUEUE_MAX_FRAMES,
            OUTPUT_QUEUE_MAX_BYTES, src->overload_episodes, queued);

        gst_droidscreencapsrc_clear_queue_locked (src);
        gst_droidscreencapsrc_free_frame (f);
        src->dropped_frames += queued + 1;
        src->dropped_bytes += dropped_bytes;
        src->waiting_for_keyframe = TRUE;
        return;
    }

    g_queue_push_tail (src->output_queue, f);
    src->queue_bytes += f->size;
    g_cond_signal (&src->output_cond);
}

/*
 * Give the held-back final access unit a duration extending to the recording
 * stop time and queue it. Requires output_lock.
 */
static void
gst_droidscreencapsrc_finalize_pending_locked (GstDroidScreenCapSrc * src)
{
    GstDroidScreenCapFrame *f = src->pending;
    GstClockTime nominal = gst_util_uint64_scale_int (GST_SECOND, 1, src->fps);
    gint64 stop_us;
    gint64 rel_us;

    if (f == NULL)
        return;
    src->pending = NULL;

    stop_us = src->stop_time_us != 0 ? src->stop_time_us : g_get_monotonic_time ();
    rel_us = stop_us - src->first_pts_us;

    if (rel_us > 0 && (GstClockTime) rel_us * 1000 > f->pts) {
        f->duration = (GstClockTime) rel_us * 1000 - f->pts;
        GST_INFO_OBJECT (src, "final frame PTS %" GST_TIME_FORMAT
            " duration %" GST_TIME_FORMAT " (to recording stop)",
            GST_TIME_ARGS (f->pts), GST_TIME_ARGS (f->duration));
    } else {
        f->duration = nominal;
        GST_WARNING_OBJECT (src, "recording stop time is not later than the "
            "final PTS %" GST_TIME_FORMAT "; using one nominal frame period %"
            GST_TIME_FORMAT, GST_TIME_ARGS (f->pts), GST_TIME_ARGS (nominal));
    }

    gst_droidscreencapsrc_enqueue_ordinary_locked (src, f);
}

/* ----------------------------------------------------------------
 * Properties
 * ---------------------------------------------------------------- */

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

/* ----------------------------------------------------------------
 * Encoder callbacks (encoder drain thread). They copy data, take
 * output_lock briefly and never block on downstream.
 * ---------------------------------------------------------------- */

static void
gst_droidscreencapsrc_data_available (void *user,
    const ScreenCaptureSurfaceEncodedFrame *frame)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (user);
    GstDroidScreenCapFrame *f;
    gint64 rel_us;

    GST_LOG_OBJECT (src, "data_available: %" G_GSIZE_FORMAT " bytes, pts=%"
        G_GINT64_FORMAT "us sync=%d codec_config=%d flags=0x%x",
        (gsize) frame->size, frame->timestamp_us, frame->sync,
        frame->codec_config, frame->flags);

    g_mutex_lock (&src->output_lock);

    if (!src->running || src->flushing || src->error != 0) {
        g_mutex_unlock (&src->output_lock);
        return;
    }

    if (frame->codec_config) {
        /* Retain the newest SPS/PPS; it is prepended in-band to sync frames. */
        g_free (src->config_data);
        src->config_data = g_memdup2 (frame->data, frame->size);
        src->config_size = frame->size;
        GST_INFO_OBJECT (src, "retained codec config (%" G_GSIZE_FORMAT
            " bytes)", (gsize) frame->size);
        g_mutex_unlock (&src->output_lock);
        return;
    }

    /* Timestamp origin and monotonicity (ordinary frames only). */
    if (!src->have_first_pts) {
        src->have_first_pts = TRUE;
        src->first_pts_us = frame->timestamp_us;
        src->last_pts_us = frame->timestamp_us;
        GST_INFO_OBJECT (src, "timestamp origin: codec pts %" G_GINT64_FORMAT
            "us", src->first_pts_us);
    } else if (frame->timestamp_us < src->last_pts_us) {
        GST_ERROR_OBJECT (src, "regressing codec PTS %" G_GINT64_FORMAT
            "us after %" G_GINT64_FORMAT "us", frame->timestamp_us,
            src->last_pts_us);
        gst_droidscreencapsrc_set_error_locked (src, -EINVAL);
        g_mutex_unlock (&src->output_lock);
        return;
    } else if (frame->timestamp_us == src->last_pts_us) {
        /* Two pictures cannot share a presentation time. The encoder has
         * been observed to emit its first IDR twice; muxers reject a
         * zero-duration sample. Keep the first, drop the repeat. */
        src->duplicate_pts_frames++;
        GST_WARNING_OBJECT (src, "discarding access unit repeating codec PTS %"
            G_GINT64_FORMAT "us (%" G_GSIZE_FORMAT " bytes, sync=%d)",
            frame->timestamp_us, (gsize) frame->size, frame->sync);
        g_mutex_unlock (&src->output_lock);
        return;
    }

    rel_us = frame->timestamp_us - src->first_pts_us;
    if (rel_us < 0 || rel_us > G_MAXINT64 / 1000) {
        GST_ERROR_OBJECT (src, "codec PTS %" G_GINT64_FORMAT
            "us is not representable relative to origin %" G_GINT64_FORMAT,
            frame->timestamp_us, src->first_pts_us);
        gst_droidscreencapsrc_set_error_locked (src, -ERANGE);
        g_mutex_unlock (&src->output_lock);
        return;
    }
    src->last_pts_us = frame->timestamp_us;

    f = g_new0 (GstDroidScreenCapFrame, 1);
    if (frame->sync) {
        f->data = gst_droidscreencapsrc_copy_sync_frame_locked (src,
            frame->data, frame->size, &f->size);
    } else {
        f->data = g_memdup2 (frame->data, frame->size);
        f->size = frame->size;
    }
    f->pts = (GstClockTime) rel_us * 1000;
    f->duration = GST_CLOCK_TIME_NONE;
    f->sync = frame->sync;

    /* One-frame lookahead: the previous frame's duration is the gap to
     * this one. */
    if (src->pending) {
        GstDroidScreenCapFrame *prev = src->pending;
        src->pending = NULL;
        prev->duration = f->pts > prev->pts ? f->pts - prev->pts : 0;
        gst_droidscreencapsrc_enqueue_ordinary_locked (src, prev);
    }

    if (src->waiting_for_keyframe) {
        if (!f->sync) {
            src->dropped_frames++;
            src->dropped_bytes += f->size;
            gst_droidscreencapsrc_free_frame (f);
            g_mutex_unlock (&src->output_lock);
            return;
        }
        GST_WARNING_OBJECT (src, "overload episode %u ended: dropped %u "
            "frames / %" G_GSIZE_FORMAT " bytes; resuming at keyframe PTS %"
            GST_TIME_FORMAT, src->overload_episodes, src->dropped_frames,
            src->dropped_bytes, GST_TIME_ARGS (f->pts));
        src->waiting_for_keyframe = FALSE;
    }

    src->pending = f;
    g_mutex_unlock (&src->output_lock);
}

static void
gst_droidscreencapsrc_format_changed (void *user, int width, int height)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (user);

    GST_INFO_OBJECT (src, "encoder output format %dx%d", width, height);
}

static void
gst_droidscreencapsrc_error (void *user, int err)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (user);

    GST_ERROR_OBJECT (src, "encoder error %d (0x%x)", err, (unsigned) -err);

    g_mutex_lock (&src->output_lock);
    gst_droidscreencapsrc_set_error_locked (src, err);
    g_mutex_unlock (&src->output_lock);
}

static void
gst_droidscreencapsrc_eos (void *user)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (user);

    GST_INFO_OBJECT (src, "encoder reached EOS");

    g_mutex_lock (&src->output_lock);
    if (!src->eos) {
        gst_droidscreencapsrc_finalize_pending_locked (src);
        src->eos = TRUE;
    }
    g_cond_broadcast (&src->output_cond);
    g_mutex_unlock (&src->output_lock);
}

/* ----------------------------------------------------------------
 * EOS interception
 * ---------------------------------------------------------------- */

/*
 * GstBaseSrc handles a downstream EOS from send_event() by flushing and
 * marking a forced EOS that bypasses create(). That would discard the codec's
 * final access units. Handle EOS here instead: mark the request and let the
 * streaming thread finish the encoder and drain the remaining data.
 * This runs with the element STATE_LOCK held and must not block.
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
                src->stop_time_us = g_get_monotonic_time ();
                GST_INFO_OBJECT (src, "EOS requested; finishing encoder "
                    "gracefully");
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

/* ----------------------------------------------------------------
 * GstBaseSrc vmethods
 * ---------------------------------------------------------------- */

static GstCaps *
gst_droidscreencapsrc_get_caps (GstBaseSrc * bsrc, GstCaps * filter)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (bsrc);
    GstCaps *caps;

    if (src->enc_width > 0 && src->enc_height > 0) {
        caps = gst_caps_new_simple ("video/x-h264",
            "stream-format", G_TYPE_STRING, "byte-stream",
            "alignment", G_TYPE_STRING, "au",
            "width", G_TYPE_INT, src->enc_width,
            "height", G_TYPE_INT, src->enc_height,
            "framerate", GST_TYPE_FRACTION, src->fps, 1,
            NULL);
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
    ScreenCaptureSurfaceEncoderCallbacks cb;
    gint width = src->width;
    gint height = src->height;

    GST_DEBUG_OBJECT (src, "start");

    g_mutex_lock (&src->output_lock);
    gst_droidscreencapsrc_reset_run_state_locked (src);
    src->running = FALSE;
    src->flushing = FALSE;
    g_cond_broadcast (&src->output_cond);
    g_mutex_unlock (&src->output_lock);

    /* 1. Dimensions: explicit, or discovered from the capture service. */
    if (width <= 0 || height <= 0) {
        int w = 0, h = 0;

        if (droid_media_screen_capture_get_dimensions (&w, &h) == 0 &&
            w > 0 && h > 0) {
            width = w;
            height = h;
            GST_INFO_OBJECT (src, "discovered display dimensions %dx%d",
                width, height);
        } else {
            GST_ELEMENT_ERROR (src, RESOURCE, SETTINGS,
                ("Screen capture dimensions are unknown"),
                ("sailfish.screencap published no dimensions; set the "
                 "width and height properties explicitly"));
            return FALSE;
        }
    }

    if (src->target_bitrate <= 0 || src->fps <= 0) {
        GST_ELEMENT_ERROR (src, RESOURCE, SETTINGS,
            ("Invalid encoder configuration"),
            ("target-bitrate=%d fps=%d", src->target_bitrate, src->fps));
        return FALSE;
    }

    src->enc_width = width;
    src->enc_height = height;

    /* 2. Create the Surface encoder. */
    memset (&cb, 0, sizeof (cb));
    cb.data_available = gst_droidscreencapsrc_data_available;
    cb.format_changed = gst_droidscreencapsrc_format_changed;
    cb.error = gst_droidscreencapsrc_error;
    cb.eos = gst_droidscreencapsrc_eos;

    src->encoder = screen_capture_surface_encoder_new (width, height,
        src->target_bitrate, src->fps, &cb, src);
    if (src->encoder == NULL) {
        src->enc_width = 0;
        src->enc_height = 0;
        GST_ELEMENT_ERROR (src, LIBRARY, INIT,
            ("Failed to create the hardware AVC Surface encoder"),
            ("%dx%d @%d fps, %d bps", width, height, src->fps,
             src->target_bitrate));
        return FALSE;
    }

    /* 3. Callbacks may fire as soon as the codec starts; accept them. */
    g_mutex_lock (&src->output_lock);
    src->running = TRUE;
    g_mutex_unlock (&src->output_lock);

    /* 4. Start MediaCodec and publish the producer generation. */
    if (!screen_capture_surface_encoder_start (src->encoder)) {
        g_mutex_lock (&src->output_lock);
        src->running = FALSE;
        g_mutex_unlock (&src->output_lock);

        screen_capture_surface_encoder_destroy (src->encoder);
        src->encoder = NULL;

        g_mutex_lock (&src->output_lock);
        gst_droidscreencapsrc_reset_run_state_locked (src);
        g_mutex_unlock (&src->output_lock);
        src->enc_width = 0;
        src->enc_height = 0;

        GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
            ("Failed to start screen capture"),
            ("Encoder start or sailfish.screencap producer registration "
             "failed for %dx%d; check that lipstick capture is enabled and "
             "the dimensions match the display", width, height));
        return FALSE;
    }

    GST_INFO_OBJECT (src, "started: %dx%d @%d fps, %d bps, queue bound %d "
        "frames / %d bytes", width, height, src->fps, src->target_bitrate,
        OUTPUT_QUEUE_MAX_FRAMES, OUTPUT_QUEUE_MAX_BYTES);
    return TRUE;
}

static gboolean
gst_droidscreencapsrc_stop (GstBaseSrc * bsrc)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (bsrc);

    GST_DEBUG_OBJECT (src, "stop");

    /* 1. Refuse new data and wake create(). */
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

    /* 2. Bounded immediate stop, without holding output_lock: the drain
     * thread may be inside a callback that needs it. If a graceful finish is
     * still in progress on the streaming thread, this waits for it. */
    if (src->encoder) {
        screen_capture_surface_encoder_stop (src->encoder);
        screen_capture_surface_encoder_destroy (src->encoder);
        src->encoder = NULL;
    }

    /* 3. Discard everything for a clean restart. */
    g_mutex_lock (&src->output_lock);
    gst_droidscreencapsrc_reset_run_state_locked (src);
    src->flushing = FALSE;
    g_mutex_unlock (&src->output_lock);

    src->enc_width = 0;
    src->enc_height = 0;

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

/* ----------------------------------------------------------------
 * GstPushSrc create() — streaming thread
 * ---------------------------------------------------------------- */

/* Perform the graceful encoder finish. Called without output_lock. */
static void
gst_droidscreencapsrc_do_finish (GstDroidScreenCapSrc * src)
{
    gboolean ok = FALSE;

    if (src->encoder) {
        GST_INFO_OBJECT (src, "finishing encoder (timeout %d ms)",
            FINISH_TIMEOUT_MS);
        ok = screen_capture_surface_encoder_finish (src->encoder,
            FINISH_TIMEOUT_MS);
    }

    g_mutex_lock (&src->output_lock);
    if (!ok) {
        GST_WARNING_OBJECT (src, "encoder did not reach codec EOS; "
            "finalizing with the data received so far");
    }
    if (!src->eos) {
        /* The drain thread has been joined; nothing more can arrive. */
        gst_droidscreencapsrc_finalize_pending_locked (src);
        src->eos = TRUE;
    }
    g_cond_broadcast (&src->output_cond);
    g_mutex_unlock (&src->output_lock);
}

static GstFlowReturn
gst_droidscreencapsrc_create (GstPushSrc * psrc, GstBuffer ** outbuf)
{
    GstDroidScreenCapSrc *src = GST_DROIDSCREENCAPSRC (psrc);
    GstDroidScreenCapFrame *f = NULL;
    GstBuffer *buf;
    GstMapInfo info;

    g_mutex_lock (&src->output_lock);

    for (;;) {
        if (src->flushing) {
            g_mutex_unlock (&src->output_lock);
            return GST_FLOW_FLUSHING;
        }

        if (!g_queue_is_empty (src->output_queue)) {
            f = (GstDroidScreenCapFrame *) g_queue_pop_head (src->output_queue);
            src->queue_bytes -= f->size;
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

    g_mutex_unlock (&src->output_lock);

    buf = gst_buffer_new_allocate (NULL, f->size, NULL);
    if (buf == NULL || !gst_buffer_map (buf, &info, GST_MAP_WRITE)) {
        gsize size = f->size;

        if (buf)
            gst_buffer_unref (buf);
        gst_droidscreencapsrc_free_frame (f);
        GST_ELEMENT_ERROR (src, RESOURCE, NO_SPACE_LEFT, (NULL),
            ("cannot allocate %" G_GSIZE_FORMAT " byte buffer", size));
        return GST_FLOW_ERROR;
    }
    memcpy (info.data, f->data, f->size);
    gst_buffer_unmap (buf, &info);

    GST_BUFFER_PTS (buf) = f->pts;
    GST_BUFFER_DTS (buf) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION (buf) = f->duration;

    if (f->sync)
        GST_BUFFER_FLAG_UNSET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
    else
        GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
    src->frames_out++;

    GST_DEBUG_OBJECT (src, "pushing access unit: %" G_GSIZE_FORMAT " bytes, "
        "PTS %" GST_TIME_FORMAT " duration %" GST_TIME_FORMAT " sync=%d",
        f->size, GST_TIME_ARGS (f->pts), GST_TIME_ARGS (f->duration), f->sync);

    gst_droidscreencapsrc_free_frame (f);

    *outbuf = buf;
    return GST_FLOW_OK;
}
