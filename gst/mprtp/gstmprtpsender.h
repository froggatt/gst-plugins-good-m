/* GStreamer
 * Copyright (C) 2015 FIXME <fixme@example.com>
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

#ifndef _GST_MPRTPSENDER_H_
#define _GST_MPRTPSENDER_H_

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_MPRTPSENDER   (gst_mprtpsender_get_type())
#define GST_MPRTPSENDER(obj)   (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_MPRTPSENDER,GstMprtpsender))
#define GST_MPRTPSENDER_CLASS(klass)   (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_MPRTPSENDER,GstMprtpsenderClass))
#define GST_IS_MPRTPSENDER(obj)   (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_MPRTPSENDER))
#define GST_IS_MPRTPSENDER_CLASS(obj)   (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_MPRTPSENDER))

typedef struct _GstMprtpsender GstMprtpsender;
typedef struct _GstMprtpsenderClass GstMprtpsenderClass;
typedef struct _GstMprtpsenderPrivate GstMprtpsenderPrivate;

struct _GstMprtpsender
{
  GstElement     base_mprtpsender;
  GRWLock        rwmutex;
  guint8         ext_header_id;
  GList*         subflows;
  GList*         iterator;
  GstPad*        mprtcp_rr_sinkpad;
  GstPad*        mprtp_sinkpad;
  GstPad*        rtcp_srcpad;
  GstPad*        mprtcp_sr_sinkpad;
  GstPad*        rtcp_sinkpad;

  //GQueue        *events;
  //GstTask       *eventing;
  //GRecMutex      eventing_mutex;
  GstMprtpsenderPrivate* priv;
};

struct _GstMprtpsenderClass
{
  GstElementClass base_mprtpsender_class;
};

GType gst_mprtpsender_get_type (void);

G_END_DECLS

#endif //_GST_MPRTPSENDER_H_
