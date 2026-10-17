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
#include "droidmediascreencapture.h"

G_BEGIN_DECLS

#define GST_TYPE_DROIDSCREENCAPSRC \
  (gst_droidscreencapsrc_get_type())
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

struct _GstDroidScreenCapSrc
{
  GstPushSrc parent;

  gint width;
  gint height;
  gint target_bitrate;
  gint fps;

  /* between start() and stop() */
  DroidMediaScreenCapture *capture;

  /* Protects everything below. Never held while calling into capture. */
  GMutex output_lock;
  GCond output_cond;

  GQueue *output_queue;
  gsize queue_bytes;
  guint8 *config_data;          /* newest SPS/PPS */
  gsize config_size;

  /* PTS = ts - first_ts, both in CLOCK_MONOTONIC ns */
  gboolean have_first_pts;
  gint64 first_ts;
  gint64 last_ts;

  /* overload: drop up to the next IDR */
  gboolean waiting_for_keyframe;
  guint dropped_frames;
  gsize dropped_bytes;
  guint overload_episodes;
  guint duplicate_pts_frames;

  gboolean running;
  gboolean flushing;
  gboolean eos_requested;
  gboolean finish_started;
  gboolean eos;                 /* everything left is in output_queue */
  gint error;
  gboolean error_posted;

  guint64 frames_out;
};

struct _GstDroidScreenCapSrcClass
{
  GstPushSrcClass parent_class;
};

GType gst_droidscreencapsrc_get_type (void);

G_END_DECLS

#endif /* __GST_DROIDSCREENCAPSRC_H__ */
