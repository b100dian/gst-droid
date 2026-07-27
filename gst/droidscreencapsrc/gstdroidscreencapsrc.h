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

struct _GstDroidScreenCapSrc {
    GstPushSrc parent;

    /* Capture consumer */
    void *queue;

    /* Encoder pipeline */
    void *encoder;

    /* Properties */
    gint target_bitrate;
    gint fps;
    gint color_format;

    /* Output queue — filled by encoder callback, drained by create() */
    GMutex output_lock;
    GCond output_cond;
    GQueue *output_queue;

    gboolean eos;
    gboolean running;
};

struct _GstDroidScreenCapSrcClass {
    GstPushSrcClass parent_class;
};

GType gst_droidscreencapsrc_get_type(void);

G_END_DECLS

#endif /* __GST_DROIDSCREENCAPSRC_H__ */