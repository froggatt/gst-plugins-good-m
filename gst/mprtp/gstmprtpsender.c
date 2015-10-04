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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstmprtpsender
 *
 * The mprtpsender element does FIXME stuff.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v fakesrc ! mprtpsender ! FIXME ! fakesink
 * ]|
 * FIXME Describe what the pipeline does.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <gst/gst.h>
#include "gstmprtpsender.h"
#include "mprtpspath.h"
#include "gstmprtcpbuffer.h"
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (gst_mprtpsender_debug_category);
#define GST_CAT_DEFAULT gst_mprtpsender_debug_category

#define THIS_WRITELOCK(this) (g_rw_lock_writer_lock(&this->rwmutex))
#define THIS_WRITEUNLOCK(this) (g_rw_lock_writer_unlock(&this->rwmutex))
#define THIS_READLOCK(this) (g_rw_lock_reader_lock(&this->rwmutex))
#define THIS_READUNLOCK(this) (g_rw_lock_reader_unlock(&this->rwmutex))

#define PACKET_IS_RTP_OR_RTCP(b) (b > 0x7f && b < 0xc0)
#define PACKET_IS_DTLS(b) (b > 0x13 && b < 0x40)
#define PACKET_IS_RTCP(b) (b > 192 && b < 223)


static void gst_mprtpsender_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_mprtpsender_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_mprtpsender_dispose (GObject * object);
static void gst_mprtpsender_finalize (GObject * object);

static GstPad *gst_mprtpsender_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps);
static void gst_mprtpsender_release_pad (GstElement * element, GstPad * pad);
static GstStateChangeReturn
gst_mprtpsender_change_state (GstElement * element, GstStateChange transition);
//static void gst_mprtpsender_eventing_run (void *data);
static gboolean gst_mprtpsender_query (GstElement * element, GstQuery * query);

static GstPadLinkReturn gst_mprtpsender_src_link (GstPad * pad,
    GstObject * parent, GstPad * peer);
static void gst_mprtpsender_src_unlink (GstPad * pad, GstObject * parent);

static GstFlowReturn gst_mprtpsender_mprtp_sink_chain (GstPad * pad,
    GstObject * parent, GstBuffer * buffer);

static GstFlowReturn
gst_mprtpsender_mprtcp_sink_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buf);

static gboolean gst_mprtpsender_mprtp_sink_event_handler (GstPad * pad,
    GstObject * parent, GstEvent * event);

typedef struct
{
  GstPad *outpad;
  guint8 state;
  guint8 id;

  //GstBuffer *report_buf;
  GstBuffer *buf_report_rr;
  GstBuffer *buf_report_sr;
  GstBuffer *buf_report_xr;

  gboolean report_started;
  GstClockTime report_interval_time;
  GstClockTime report_time;
  GstClockTime urgent_report_requested_time;
  gboolean allow_early_report;
  GstClock *sysclock;
} Subflow;

static gboolean _do_subflow_report_now (Subflow * this, gboolean urgent);
static void _merge_xr_riports (Subflow * this, GstRTCPXR_RFC7243 * new_xr);
static void _merge_sr_riports (Subflow * this, GstRTCPSRBlock * new_sr);
static void _merge_rr_riports (Subflow * this, GstRTCPRRBlock * new_rr);
static gboolean _process_xr_rfc7243_report (Subflow * this, GstBuffer * buf,
    GstMPRTCPSubflowBlock * block);
static gboolean _process_sr_report (Subflow * this, GstBuffer * buf,
    GstMPRTCPSubflowBlock * block);
static gboolean _process_rr_report (Subflow * this, GstBuffer * buf,
    GstMPRTCPSubflowBlock * block);
static gboolean _process_report (GstMprtpsender * this, GstBuffer * buf,
    Subflow ** subflow, gboolean * urgent);
static GstBuffer *_assemble_report (Subflow * this);

static gboolean
_select_subflow (GstMprtpsender * this, guint8 id, Subflow ** result);

enum
{
  PROP_0,
  PROP_EXT_HEADER_ID,
  PROP_PIVOT_OUTPAD,
};

/* pad templates */

static GstStaticPadTemplate gst_mprtpsender_src_template =
GST_STATIC_PAD_TEMPLATE ("src_%u",
    GST_PAD_SRC,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);


static GstStaticPadTemplate gst_mprtpsender_mprtp_sink_template =
GST_STATIC_PAD_TEMPLATE ("mprtp_sink",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);


static GstStaticPadTemplate gst_mprtpsender_mprtcp_rr_sink_template =
GST_STATIC_PAD_TEMPLATE ("mprtcp_rr_sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
//    GST_STATIC_CAPS ("application/x-rtcp")
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate gst_mprtpsender_mprtcp_sr_sink_template =
GST_STATIC_PAD_TEMPLATE ("mprtcp_sr_sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
//    GST_STATIC_CAPS ("application/x-rtcp")
    GST_STATIC_CAPS_ANY);

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstMprtpsender, gst_mprtpsender, GST_TYPE_ELEMENT,
    GST_DEBUG_CATEGORY_INIT (gst_mprtpsender_debug_category, "mprtpsender", 0,
        "debug category for mprtpsender element"));

#define GST_MPRTPSENDER_GET_PRIVATE(obj)  \
    (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_MPRTPSENDER, GstMprtpsenderPrivate))

struct _GstMprtpsenderPrivate
{
  gboolean have_same_caps;

  GstBufferPool *pool;
  gboolean pool_active;
  GstAllocator *allocator;
  GstAllocationParams params;
  GstQuery *query;
};


gboolean
gst_mprtpsender_mprtp_sink_event_handler (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstMprtpsender *this;
  gboolean result = TRUE;
  GList *it;
  Subflow *subflow;
  const gchar *stream_id;
  const GstSegment *segment;
  GstCaps *caps;

  this = GST_MPRTPSENDER (parent);
  THIS_WRITELOCK (this);
  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_STREAM_START:
      if (this->event_stream_start != NULL) {
        gst_event_unref (this->event_stream_start);
      }
      gst_event_parse_stream_start (event, &stream_id);
      this->event_stream_start = gst_event_new_stream_start (stream_id);
      goto sending;
    case GST_EVENT_SEGMENT:
      if (this->event_segment != NULL) {
        gst_event_unref (this->event_segment);
      }
      gst_event_parse_segment (event, &segment);
      this->event_segment = gst_event_new_segment (segment);
      goto sending;
    case GST_EVENT_CAPS:
      if (this->event_caps != NULL) {
        gst_event_unref (this->event_caps);
      }
      gst_event_parse_caps (event, &caps);
      this->event_caps = gst_event_new_caps (caps);
    sending:
      for (subflow = NULL, it = this->subflows; it != NULL; it = it->next) {
        subflow = it->data;
        result &= gst_pad_push_event (subflow->outpad, gst_event_copy (event));
      }
      result &= gst_pad_event_default (pad, parent, event);
      break;
    default:
      result = gst_pad_event_default (pad, parent, event);
  }

  THIS_WRITEUNLOCK (this);
  return result;
}


static void
gst_mprtpsender_class_init (GstMprtpsenderClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GstMprtpsenderPrivate));
  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mprtpsender_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mprtpsender_mprtcp_sr_sink_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mprtpsender_mprtcp_rr_sink_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mprtpsender_mprtp_sink_template));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "FIXME Long name", "Generic", "FIXME Description",
      "FIXME <fixme@example.com>");

  gobject_class->set_property = gst_mprtpsender_set_property;
  gobject_class->get_property = gst_mprtpsender_get_property;
  gobject_class->dispose = gst_mprtpsender_dispose;
  gobject_class->finalize = gst_mprtpsender_finalize;
  element_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_mprtpsender_request_new_pad);
  element_class->release_pad = GST_DEBUG_FUNCPTR (gst_mprtpsender_release_pad);
  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_mprtpsender_change_state);
  element_class->query = GST_DEBUG_FUNCPTR (gst_mprtpsender_query);

  g_object_class_install_property (gobject_class, PROP_EXT_HEADER_ID,
      g_param_spec_uint ("ext-header-id",
          "Set or get the id for the RTP extension",
          "Sets or gets the id for the extension header the MpRTP based on", 0,
          15, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PIVOT_OUTPAD,
      g_param_spec_uint ("pivot-outpad",
          "The id of the subflow sets to pivot for non-mp packets.",
          "The id of the subflow sets to pivot for non-mp packets. (DTLS, RTCP, Others)",
          0, 255, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));
}



static void
gst_mprtpsender_init (GstMprtpsender * mprtpsender)
{

  mprtpsender->mprtcp_rr_sinkpad =
      gst_pad_new_from_static_template
      (&gst_mprtpsender_mprtcp_rr_sink_template, "mprtcp_rr_sink");
  gst_pad_set_chain_function (mprtpsender->mprtcp_rr_sinkpad,
      GST_DEBUG_FUNCPTR (gst_mprtpsender_mprtcp_sink_chain));
  gst_element_add_pad (GST_ELEMENT (mprtpsender),
      mprtpsender->mprtcp_rr_sinkpad);

  mprtpsender->mprtcp_sr_sinkpad =
      gst_pad_new_from_static_template
      (&gst_mprtpsender_mprtcp_sr_sink_template, "mprtcp_sr_sink");
  gst_pad_set_chain_function (mprtpsender->mprtcp_sr_sinkpad,
      GST_DEBUG_FUNCPTR (gst_mprtpsender_mprtcp_sink_chain));
  gst_element_add_pad (GST_ELEMENT (mprtpsender),
      mprtpsender->mprtcp_sr_sinkpad);

  mprtpsender->mprtp_sinkpad =
      gst_pad_new_from_static_template (&gst_mprtpsender_mprtp_sink_template,
      "mprtp_sink");
  gst_pad_set_chain_function (mprtpsender->mprtp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_mprtpsender_mprtp_sink_chain));
  gst_pad_set_event_function (mprtpsender->mprtp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_mprtpsender_mprtp_sink_event_handler));

  gst_element_add_pad (GST_ELEMENT (mprtpsender), mprtpsender->mprtp_sinkpad);

//  GST_PAD_SET_PROXY_CAPS (mprtpsender->mprtp_sinkpad);
//  GST_PAD_SET_PROXY_ALLOCATION (mprtpsender->mprtp_sinkpad);

  mprtpsender->ext_header_id = MPRTP_DEFAULT_EXTENSION_HEADER_ID;
  mprtpsender->pivot_outpad = NULL;
  mprtpsender->event_segment = NULL;
  mprtpsender->event_caps = NULL;
  mprtpsender->event_stream_start = NULL;
  //mprtpsender->events = g_queue_new();
  g_rw_lock_init (&mprtpsender->rwmutex);
}

void
gst_mprtpsender_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMprtpsender *this = GST_MPRTPSENDER (object);
  guint8 subflow_id;
  Subflow *subflow;
  GST_DEBUG_OBJECT (this, "set_property");

  switch (property_id) {
    case PROP_EXT_HEADER_ID:
      THIS_WRITELOCK (this);
      this->ext_header_id = (guint8) g_value_get_uint (value);
      THIS_WRITEUNLOCK (this);
      break;
    case PROP_PIVOT_OUTPAD:
      THIS_WRITELOCK (this);
      subflow_id = (guint8) g_value_get_uint (value);
      if (_select_subflow (this, subflow_id, &subflow)) {
        this->pivot_outpad = subflow->outpad;
      } else {
        this->pivot_outpad = NULL;
      }
      THIS_WRITEUNLOCK (this);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_mprtpsender_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstMprtpsender *this = GST_MPRTPSENDER (object);

  GST_DEBUG_OBJECT (this, "get_property");

  switch (property_id) {
    case PROP_EXT_HEADER_ID:
      THIS_READLOCK (this);
      g_value_set_uint (value, (guint) this->ext_header_id);
      THIS_READUNLOCK (this);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_mprtpsender_dispose (GObject * object)
{
  GstMprtpsender *mprtpsender = GST_MPRTPSENDER (object);

  GST_DEBUG_OBJECT (mprtpsender, "dispose");

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (gst_mprtpsender_parent_class)->dispose (object);
}

void
gst_mprtpsender_finalize (GObject * object)
{
  GstMprtpsender *mprtpsender = GST_MPRTPSENDER (object);

  GST_DEBUG_OBJECT (mprtpsender, "finalize");

  /* clean up object here */

  G_OBJECT_CLASS (gst_mprtpsender_parent_class)->finalize (object);
}



static GstPad *
gst_mprtpsender_request_new_pad (GstElement * element, GstPadTemplate * templ,
    const gchar * name, const GstCaps * caps)
{

  GstPad *srcpad;
  GstMprtpsender *this;
  guint8 subflow_id;
  Subflow *subflow;

  this = GST_MPRTPSENDER (element);
  GST_DEBUG_OBJECT (this, "requesting pad");

  sscanf (name, "src_%hhu", &subflow_id);
  THIS_WRITELOCK (this);
  subflow = (Subflow *) g_malloc0 (sizeof (Subflow));

  srcpad = gst_pad_new_from_template (templ, name);
//  GST_PAD_SET_PROXY_CAPS (srcpad);
//  GST_PAD_SET_PROXY_ALLOCATION (srcpad);

  gst_pad_set_link_function (srcpad,
      GST_DEBUG_FUNCPTR (gst_mprtpsender_src_link));
  gst_pad_set_unlink_function (srcpad,
      GST_DEBUG_FUNCPTR (gst_mprtpsender_src_unlink));
  subflow->id = subflow_id;
  subflow->outpad = srcpad;
  subflow->state = 0;
  subflow->sysclock = gst_system_clock_obtain ();
  subflow->allow_early_report = TRUE;
  this->subflows = g_list_prepend (this->subflows, subflow);
  THIS_WRITEUNLOCK (this);

  gst_pad_set_active (srcpad, TRUE);

  gst_element_add_pad (GST_ELEMENT (this), srcpad);

  return srcpad;
}

static void
gst_mprtpsender_release_pad (GstElement * element, GstPad * pad)
{

}



static GstStateChangeReturn
gst_mprtpsender_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  g_return_val_if_fail (GST_IS_MPRTPSENDER (element), GST_STATE_CHANGE_FAILURE);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      //gst_task_start (mprtpsender->eventing);
      break;
    default:
      break;
  }

  ret =
      GST_ELEMENT_CLASS (gst_mprtpsender_parent_class)->change_state (element,
      transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

  return ret;
}

static gboolean
gst_mprtpsender_query (GstElement * element, GstQuery * query)
{
  GstMprtpsender *mprtpsender = GST_MPRTPSENDER (element);
  gboolean ret;

  GST_DEBUG_OBJECT (mprtpsender, "query");
  switch (GST_QUERY_TYPE (query)) {
    default:
      ret =
          GST_ELEMENT_CLASS (gst_mprtpsender_parent_class)->query (element,
          query);
      break;
  }

  return ret;
}


static GstPadLinkReturn
gst_mprtpsender_src_link (GstPad * pad, GstObject * parent, GstPad * peer)
{
  GstMprtpsender *this;
  GList *it;
  Subflow *subflow;
  GstPadLinkReturn result = GST_PAD_LINK_OK;

  this = GST_MPRTPSENDER (parent);
  GST_DEBUG_OBJECT (this, "link");
  THIS_READLOCK (this);

  for (subflow = NULL, it = this->subflows; it != NULL; it = it->next) {
    subflow = it->data;
    if (subflow->outpad == pad) {
      break;
    }
    subflow = NULL;
  }
  if (subflow == NULL) {
    goto gst_mprtpsender_src_link_done;
  }
  subflow->state = 0;
  if (this->event_stream_start != NULL) {
    gst_pad_push_event (subflow->outpad,
        gst_event_copy (this->event_stream_start));
  }
  if (this->event_caps != NULL) {
    gst_pad_push_event (subflow->outpad, gst_event_copy (this->event_caps));
  }
  if (this->event_segment != NULL) {
    gst_pad_push_event (subflow->outpad, gst_event_copy (this->event_segment));
  }

gst_mprtpsender_src_link_done:
  THIS_READUNLOCK (this);
  return result;
}

static void
gst_mprtpsender_src_unlink (GstPad * pad, GstObject * parent)
{
  GstMprtpsender *mprtpsender;
  GList *it;
  Subflow *subflow;

  mprtpsender = GST_MPRTPSENDER (parent);
  GST_DEBUG_OBJECT (mprtpsender, "unlink");
  THIS_WRITELOCK (mprtpsender);

  for (subflow = NULL, it = mprtpsender->subflows; it != NULL; it = it->next) {
    subflow = it->data;
    if (subflow->outpad == pad) {
      break;
    }
    subflow = NULL;
  }
  if (subflow == NULL) {
    goto gst_mprtpsender_src_unlink_done;
  }
  gst_object_unref (subflow->sysclock);
  mprtpsender->subflows = g_list_remove (mprtpsender->subflows, subflow);
gst_mprtpsender_src_unlink_done:
  THIS_WRITEUNLOCK (mprtpsender);
}

typedef enum
{
  PACKET_IS_MPRTP,
  PACKET_IS_MPRTCP,
  PACKET_IS_NOT_MP,
} PacketTypes;

static PacketTypes
_get_packet_mptype (GstMprtpsender * this,
    GstBuffer * buf, GstMapInfo * info, guint8 * subflow_id)
{

  guint8 first_byte, second_byte;
  PacketTypes result = PACKET_IS_NOT_MP;
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  MPRTPSubflowHeaderExtension *subflow_infos = NULL;
  guint size;
  gpointer pointer;

  if (gst_buffer_extract (buf, 0, &first_byte, 1) != 1 ||
      gst_buffer_extract (buf, 1, &second_byte, 1) != 1) {
    GST_WARNING_OBJECT (this, "could not extract first byte from buffer");
    goto done;
  }
  if (PACKET_IS_DTLS (first_byte)) {
    goto done;
  }

  if (PACKET_IS_RTP_OR_RTCP (first_byte)) {
    if (PACKET_IS_RTCP (second_byte)) {
      if (second_byte != MPRTCP_PACKET_TYPE_IDENTIFIER) {
        goto done;
      }
      if (subflow_id) {
        *subflow_id = (guint8)
            g_ntohs (*((guint16 *) (info->data + 8 /*RTCP Header */  +
                    6 /*first block info until subflow id */ )));
      }
      result = PACKET_IS_MPRTCP;
      goto done;
    }

    if (G_UNLIKELY (!gst_rtp_buffer_map (buf, GST_MAP_READ, &rtp))) {
      GST_WARNING_OBJECT (this, "The RTP packet is not readable");
      goto done;
    }

    if (!gst_rtp_buffer_get_extension (&rtp)) {
      gst_rtp_buffer_unmap (&rtp);
      goto done;
    }

    if (!gst_rtp_buffer_get_extension_onebyte_header (&rtp, this->ext_header_id,
            0, &pointer, &size)) {
      gst_rtp_buffer_unmap (&rtp);
      goto done;
    }

    if (subflow_id) {
      subflow_infos = (MPRTPSubflowHeaderExtension *) pointer;
      *subflow_id = subflow_infos->id;

      //SRTP validation - it must be fail
//      gst_rtp_buffer_add_extension_onebyte_header (&rtp, 2,
//            (gpointer) subflow_infos, sizeof (*subflow_infos));
    }
    gst_rtp_buffer_unmap (&rtp);
    result = PACKET_IS_MPRTP;
    goto done;
  }


done:
  return result;
}

static GstFlowReturn
gst_mprtpsender_mprtp_sink_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buf)
{
  GstMprtpsender *this;
  GstFlowReturn result;
  GstMapInfo map;
  PacketTypes packet_type;
  guint8 subflow_id;
  Subflow *subflow;
  gint n, r;
  GstPad *outpad;

  this = GST_MPRTPSENDER (parent);
  GST_DEBUG_OBJECT (this, "RTP/MPRTP/OTHER sink");
  if (!gst_buffer_map (buf, &map, GST_MAP_READ)) {
    GST_ERROR_OBJECT (this, "Buffer is not readable");
    result = GST_FLOW_CUSTOM_ERROR;
    goto exit;
  }

  THIS_READLOCK (this);
  n = g_list_length (this->subflows);
  if (n < 1) {
    GST_ERROR_OBJECT (this, "No appropiate subflow");
    gst_buffer_unmap (buf, &map);
    result = GST_FLOW_CUSTOM_ERROR;
    goto done;
  }
  packet_type = _get_packet_mptype (this, buf, &map, &subflow_id);
  if (packet_type != PACKET_IS_NOT_MP &&
      _select_subflow (this, subflow_id, &subflow) != FALSE) {
    outpad = subflow->outpad;
  } else if (this->pivot_outpad != NULL &&
      gst_pad_is_active (this->pivot_outpad) &&
      gst_pad_is_linked (this->pivot_outpad)) {
    outpad = this->pivot_outpad;
  } else {
    if (n > 1) {
      r = g_random_int_range (0, n);
      subflow = g_list_nth (this->subflows, r)->data;
      outpad = subflow->outpad;
    } else {
      subflow = this->subflows->data;
      outpad = subflow->outpad;
    }
  }
  gst_buffer_unmap (buf, &map);
  result = gst_pad_push (outpad, buf);
done:
  THIS_READUNLOCK (this);
exit:
  return result;

}


static GstFlowReturn
gst_mprtpsender_mprtcp_sink_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buf)
{
  GstMprtpsender *this;
  GstFlowReturn result = GST_FLOW_OK;
  Subflow *subflow = NULL;
  gboolean urgent = FALSE;
  this = GST_MPRTPSENDER (parent);
  THIS_WRITELOCK (this);

  if (!_process_report (this, buf, &subflow, &urgent)) {
    GST_WARNING_OBJECT (this, "The report can not be processed");
  }
  if (_do_subflow_report_now (subflow, urgent)) {
    result = gst_pad_push (subflow->outpad, _assemble_report (subflow));
  }

  THIS_WRITEUNLOCK (this);
  return result;
}


GstBuffer *
_assemble_report (Subflow * this)
{
  GstBuffer *result;
  gpointer dataptr;
  GstMPRTCPSubflowReport report;
  guint16 length;
//  GstRTCPBuffer rtcp = GST_RTCP_BUFFER_INIT;
  guint8 src = 0;
  GstMapInfo map = GST_MAP_INFO_INIT;

  gst_mprtcp_report_init (&report);
  gst_rtcp_header_getdown (&report.header, NULL, NULL, NULL, NULL, &length,
      NULL);
  length = (length + 1) << 2;
  dataptr = g_malloc0 (length);
  memcpy (dataptr, &report, length);
  result = gst_buffer_new_wrapped (dataptr, length);
  if (this->buf_report_sr != NULL) {
    result = gst_buffer_append (result, this->buf_report_sr);
    gst_buffer_unref (this->buf_report_sr);
    this->buf_report_sr = NULL;
    ++src;
  }
  if (this->buf_report_rr != NULL) {
    result = gst_buffer_append (result, this->buf_report_rr);
    gst_buffer_unref (this->buf_report_rr);
    this->buf_report_rr = NULL;
    ++src;
  }
  if (this->buf_report_xr != NULL) {
    result = gst_buffer_append (result, this->buf_report_xr);
    gst_buffer_unref (this->buf_report_xr);
    this->buf_report_xr = NULL;
    ++src;
  }

  gst_buffer_map (result, &map, GST_MAP_WRITE);
  gst_rtcp_header_change ((GstRTCPHeader *) map.data, NULL, NULL, &src,
      NULL, NULL, NULL);
  gst_buffer_unmap (result, &map);

//  gst_rtcp_buffer_map(result, GST_MAP_READ, &rtcp);
//  gst_print_rtcp_buffer(&rtcp);
//  gst_rtcp_buffer_unmap(&rtcp);

  return result;

}

gboolean
_process_report (GstMprtpsender * this, GstBuffer * buf, Subflow ** subflow,
    gboolean * urgent)
{
  gboolean result = FALSE;
  GstMPRTCPSubflowBlock *block;
  guint16 subflow_id;
  GstMapInfo map = GST_MAP_INFO_INIT;
  guint8 block_type;
  guint8 pt;

  if (!gst_buffer_map (buf, &map, GST_MAP_READ)) {
    GST_ERROR_OBJECT (this, "Buffer is not readable");
    goto exit;
  }
  block = (GstMPRTCPSubflowBlock *) map.data;
  gst_mprtcp_block_getdown (&block->info, &block_type, NULL, &subflow_id);

  if (!_select_subflow (this, (guint8) subflow_id, subflow)) {
    GST_WARNING_OBJECT (this, "No subflow exists with id: %d", subflow_id);
    goto done;
  }
  gst_rtcp_header_getdown (&block->block_header, NULL, NULL, NULL, &pt, NULL,
      NULL);
  result = TRUE;
  if (pt == GST_RTCP_TYPE_RR) {
    *urgent = _process_rr_report (*subflow, buf, block);
  } else if (pt == GST_RTCP_TYPE_SR) {
    *urgent = _process_sr_report (*subflow, buf, block);
  } else if (pt == GST_RTCP_TYPE_XR) {
    *urgent = _process_xr_rfc7243_report (*subflow, buf, block);
  } else {
    GST_WARNING_OBJECT (this, "Not recognized report to process");
  }
done:
  gst_buffer_unmap (buf, &map);
exit:
  return result;
}

gboolean
_process_rr_report (Subflow * this, GstBuffer * buf,
    GstMPRTCPSubflowBlock * block)
{
  gboolean result = FALSE;
  guint8 fraction_lost;

  if (this->buf_report_rr == NULL) {
    this->buf_report_rr = buf;
    gst_buffer_ref (this->buf_report_rr);
  } else {
    _merge_rr_riports (this, &block->receiver_riport.blocks);
  }
  gst_rtcp_rrb_getdown (&block->receiver_riport.blocks, NULL, &fraction_lost,
      NULL, NULL, NULL, NULL, NULL);
  result = fraction_lost > 0;
  return result;
}

gboolean
_process_sr_report (Subflow * this, GstBuffer * buf,
    GstMPRTCPSubflowBlock * block)
{
  if (this->buf_report_sr == NULL) {
    this->buf_report_sr = buf;
    gst_buffer_ref (this->buf_report_sr);
  } else {
    _merge_sr_riports (this, &block->sender_riport.sender_block);
  }
  return FALSE;
}

gboolean
_process_xr_rfc7243_report (Subflow * this, GstBuffer * buf,
    GstMPRTCPSubflowBlock * block)
{
  if (this->buf_report_xr == NULL) {
    this->buf_report_xr = buf;
    gst_buffer_ref (this->buf_report_xr);
  } else {
    _merge_xr_riports (this, &block->xr_rfc7243_riport);
  }
  return TRUE;
}


void
_merge_rr_riports (Subflow * this, GstRTCPRRBlock * new_rr)
{
  guint8 old_fraction_lost;
  guint8 new_fraction_lost;
  guint32 new_cum_packet_lost;
  guint32 new_LSR, new_DLSR, new_ext_hsn, new_jitter;
  guint32 new_ssrc;
  GstMapInfo map = GST_MAP_INFO_INIT;
  GstMPRTCPSubflowBlock *block;
  GstRTCPRRBlock *rr;
  if (!gst_buffer_map (this->buf_report_rr, &map, GST_MAP_READ)) {
    GST_ERROR_OBJECT (this, "Buffer is not readable");
    return;
  }
  block = (GstMPRTCPSubflowBlock *) map.data;
  rr = &block->receiver_riport.blocks;
  gst_rtcp_rrb_getdown (new_rr, &new_ssrc, &new_fraction_lost,
      &new_cum_packet_lost, &new_ext_hsn, &new_jitter, &new_LSR, &new_DLSR);

  gst_rtcp_rrb_getdown (rr, NULL, &old_fraction_lost,
      NULL, NULL, NULL, NULL, NULL);

  new_fraction_lost += old_fraction_lost;

  gst_rtcp_rrb_setup (rr, new_ssrc, new_fraction_lost,
      new_cum_packet_lost, new_ext_hsn, new_jitter, new_LSR, new_DLSR);

  gst_buffer_unmap (this->buf_report_rr, &map);
}


void
_merge_sr_riports (Subflow * this, GstRTCPSRBlock * new_sr)
{
  guint64 new_ntp;
  guint32 new_rtp, new_pc, new_oc, old_pc, old_oc;
  GstMapInfo map = GST_MAP_INFO_INIT;
  GstMPRTCPSubflowBlock *block;
  GstRTCPSRBlock *sr;

  if (!gst_buffer_map (this->buf_report_sr, &map, GST_MAP_READ)) {
    GST_ERROR_OBJECT (this, "Buffer is not readable");
    return;
  }
  block = (GstMPRTCPSubflowBlock *) map.data;
  sr = &block->sender_riport.sender_block;

  gst_rtcp_srb_getdown (new_sr, &new_ntp, &new_rtp, &new_pc, &new_oc);
  gst_rtcp_srb_getdown (sr, NULL, NULL, &old_pc, &old_oc);
  new_pc += old_pc;
  new_oc += old_oc;
  gst_rtcp_srb_setup (sr, new_ntp, new_rtp, new_pc, new_oc);
  gst_buffer_unmap (this->buf_report_sr, &map);
}

void
_merge_xr_riports (Subflow * this, GstRTCPXR_RFC7243 * new_xr)
{
  guint32 old_db, new_db;
  GstMapInfo map = GST_MAP_INFO_INIT;
  GstMPRTCPSubflowBlock *block;
  GstRTCPXR_RFC7243 *xr;

  if (!gst_buffer_map (this->buf_report_xr, &map, GST_MAP_READ)) {
    GST_ERROR_OBJECT (this, "Buffer is not readable");
    return;
  }
  block = (GstMPRTCPSubflowBlock *) map.data;
  xr = &block->xr_rfc7243_riport;

  gst_rtcp_xr_rfc7243_getdown (xr, NULL, NULL, NULL, &old_db);
  gst_rtcp_xr_rfc7243_getdown (new_xr, NULL, NULL, NULL, &new_db);
  new_db += old_db;
  gst_rtcp_xr_rfc7243_change (xr, NULL, NULL, NULL, &new_db);

  gst_buffer_unmap (this->buf_report_xr, &map);

}

gboolean
_select_subflow (GstMprtpsender * this, guint8 id, Subflow ** result)
{
  GList *it;
  Subflow *subflow;

  for (it = this->subflows; it != NULL; it = it->next) {
    subflow = it->data;
    if (subflow->id == id) {
      *result = subflow;
      return TRUE;
    }
  }
  *result = NULL;
  return FALSE;
}


gboolean
_do_subflow_report_now (Subflow * this, gboolean urgent)
{
  GstClockTime now;
  gboolean result = FALSE;

  now = gst_clock_get_time (this->sysclock);
  if (!this->report_started) {
    this->report_interval_time = 1 * GST_SECOND;
    this->report_time = now + this->report_interval_time;
    this->report_started = TRUE;
    goto done;
  }

  if (urgent) {
    if (!this->allow_early_report) {
      goto done;
    }
    result = TRUE;
    this->allow_early_report = FALSE;
    if (this->urgent_report_requested_time < now - 20 * GST_SECOND) {
      this->urgent_report_requested_time = now;
    }

    this->report_interval_time =
        (gdouble) this->report_interval_time / 2. * g_random_double_range (.9,
        1.1);
    if (this->report_interval_time < 500 * GST_MSECOND) {
      this->report_interval_time =
          1000. * (gdouble) GST_MSECOND *g_random_double_range (.8, 1.2);;
    }
    this->report_time = now + this->report_interval_time;
    goto done;
  }

  if (now < this->report_time) {
    result = FALSE;
    goto done;
  }
  result = TRUE;
  this->allow_early_report = TRUE;
  if (now - 10 * GST_SECOND < this->urgent_report_requested_time) {
    if (urgent) {
      this->report_interval_time =
          (gdouble) this->report_interval_time / 2. * g_random_double_range (.7,
          1.3);
    } else {
      this->report_interval_time =
          (gdouble) this->report_interval_time * g_random_double_range (.7,
          1.3);
    }
    if (this->report_interval_time < 500 * GST_MSECOND) {
      this->report_interval_time =
          1000. * (gdouble) GST_MSECOND *g_random_double_range (.8, 1.2);
    }
    if (7500 * GST_MSECOND < this->report_interval_time) {
      this->report_interval_time =
          7500. * (gdouble) GST_MSECOND *g_random_double_range (.8, 1.2);
    }
    this->report_time = now + this->report_interval_time;
    goto done;
  }

  this->report_interval_time =
      (gdouble) this->report_interval_time * g_random_double_range (0.75, 2.25);
  if (7500 * GST_MSECOND < this->report_interval_time) {
    this->report_interval_time =
        7500. * (gdouble) GST_MSECOND *g_random_double_range (.8, 1.2);
  }
  this->report_time = now + this->report_interval_time;
done:
  return result;
}

#undef THIS_WRITELOCK
#undef THIS_WRITEUNLOCK
#undef THIS_READLOCK
#undef THIS_READUNLOCK
#undef PACKET_IS_RTP_OR_RTCP
#undef PACKET_IS_DTLS
