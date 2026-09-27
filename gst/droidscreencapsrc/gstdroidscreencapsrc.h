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

#ifndef __GST_DROIDSCREENCAPSRC_H__
#define __GST_DROIDSCREENCAPSRC_H__

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

G_BEGIN_DECLS

#define GST_TYPE_DROIDSCREENCAPSRC (gst_droidscreencapsrc_get_type())
#define GST_DROIDSCREENCAPSRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_DROIDSCREENCAPSRC, GstDroidScreenCapSrc))
#define GST_DROIDSCREENCAPSRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_DROIDSCREENCAPSRC, GstDroidScreenCapSrcClass))
#define GST_IS_DROIDSCREENCAPSRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_DROIDSCREENCAPSRC))
#define GST_IS_DROIDSCREENCAPSRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_DROIDSCREENCAPSRC))

typedef struct _GstDroidScreenCapSrc GstDroidScreenCapSrc;
typedef struct _GstDroidScreenCapSrcClass GstDroidScreenCapSrcClass;
typedef struct _GstDroidScreenCapFrame GstDroidScreenCapFrame;

struct _GstDroidScreenCapSrc {
    GstPushSrc parent;

    /* Properties */
    gint width;
    gint height;
    gint target_bitrate;
    gint fps;

    /* Dimensions resolved at start() (explicit or discovered) */
    gint enc_width;
    gint enc_height;

    /* ScreenCaptureSurfaceEncoder *, owned by the element between start()
     * and stop(). Only touched from the streaming/state-change threads. */
    void *encoder;

    /*
     * output_lock protects everything below. Callbacks from the encoder drain
     * thread, create() on the streaming thread, send_event() and
     * unlock()/stop() on application threads all take it. Encoder
     * start/finish/stop/destroy are never called while it is held.
     */
    GMutex output_lock;
    GCond output_cond;

    /* Finalized frames ready for create(); bounded by frames and bytes. */
    GQueue *output_queue;
    gsize queue_bytes;

    /* Newest ordinary access unit, held back until its successor arrives so
     * that duration = next_pts - pts can be assigned. */
    GstDroidScreenCapFrame *pending;

    /* Latest codec configuration (SPS/PPS), retained outside the queue so an
     * overload drop can never lose the only copy. It is prepended in-band to
     * every sync frame that does not already carry parameter sets; it is
     * never pushed as a separate buffer (see data_available). */
    guint8 *config_data;
    gsize config_size;

    /* Timestamp normalization: MediaCodec PTS are absolute CLOCK_MONOTONIC
     * microseconds; GstBuffer PTS = (pts_us - first_pts_us) * 1000. */
    gboolean have_first_pts;
    gint64 first_pts_us;
    gint64 last_pts_us;

    /* Overload handling */
    gboolean waiting_for_keyframe;
    guint dropped_frames;
    gsize dropped_bytes;
    guint overload_episodes;
    guint duplicate_pts_frames; /* AUs discarded for repeating the last PTS */

    /* Lifecycle state */
    gboolean running;        /* start() succeeded, stop() not yet run */
    gboolean flushing;       /* unlock()/stop(): create() must return */
    gboolean eos_requested;  /* downstream EOS received via send_event() */
    gboolean finish_started; /* streaming thread has begun encoder finish */
    gboolean eos;            /* all remaining data is in output_queue */
    gint64 stop_time_us;     /* CLOCK_MONOTONIC us when EOS was requested */
    gint error;              /* first encoder error, 0 if none */
    gboolean error_posted;

    /* Statistics for logs */
    guint64 frames_out;
};

struct _GstDroidScreenCapSrcClass {
    GstPushSrcClass parent_class;
};

GType gst_droidscreencapsrc_get_type(void);

G_END_DECLS

#endif /* __GST_DROIDSCREENCAPSRC_H__ */
