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
 * SECTION:element-gstmprtpscheduler
 *
 * The mprtpscheduler element does FIXME stuff.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v fakesrc ! mprtpscheduler ! FIXME ! fakesink
 * ]|
 * FIXME Describe what the pipeline does.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gst/gst.h>
#include "gstmprtpscheduler.h"
#include "mprtpspath.h"
#include "streamsplitter.h"
#include "gstmprtcpbuffer.h"
#include "smanctrler.h"
#include "sefctrler.h"

GST_DEBUG_CATEGORY_STATIC (gst_mprtpscheduler_debug_category);
#define GST_CAT_DEFAULT gst_mprtpscheduler_debug_category

#define PACKET_IS_RTP_OR_RTCP(b) (b > 0x7f && b < 0xc0)

#define THIS_WRITELOCK(this) (g_rw_lock_writer_lock(&this->rwmutex))
#define THIS_WRITEUNLOCK(this) (g_rw_lock_writer_unlock(&this->rwmutex))
#define THIS_READLOCK(this) (g_rw_lock_reader_lock(&this->rwmutex))
#define THIS_READUNLOCK(this) (g_rw_lock_reader_unlock(&this->rwmutex))

#define MPRTP_SENDER_DEFAULT_ALPHA_VALUE 0.5
#define MPRTP_SENDER_DEFAULT_BETA_VALUE 0.1
#define MPRTP_SENDER_DEFAULT_GAMMA_VALUE 0.2
#define MPRTP_DEFAULT_MAX_RETAIN_TIME_IN_MS 30


static void gst_mprtpscheduler_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_mprtpscheduler_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_mprtpscheduler_dispose (GObject * object);
static void gst_mprtpscheduler_finalize (GObject * object);

static GstStateChangeReturn
gst_mprtpscheduler_change_state (GstElement * element,
    GstStateChange transition);
static gboolean gst_mprtpscheduler_query (GstElement * element,
    GstQuery * query);

static GstFlowReturn gst_mprtpscheduler_rtp_sink_chain (GstPad * pad,
    GstObject * parent, GstBuffer * buffer);
static GstFlowReturn gst_mprtpscheduler_rtp_sink_chainlist (GstPad * pad,
    GstObject * parent, GstBufferList * list);

static GstFlowReturn gst_mprtpscheduler_mprtcp_rr_sink_chain (GstPad * pad,
    GstObject * parent, GstBuffer * outbuf);

static gboolean gst_mprtpscheduler_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query);

static void gst_mprtpscheduler_mprtcp_sender (gpointer ptr, GstBuffer * buf);

static void _join_subflow (GstMprtpscheduler * this, guint subflow_id);
static void _detach_subflow (GstMprtpscheduler * this, guint subflow_id);
static gboolean
_try_get_path (GstMprtpscheduler * this, guint16 subflow_id,
    MPRTPSPath ** result);
static void _change_path_state (GstMprtpscheduler * this, guint8 subflow_id,
    gboolean set_congested, gboolean set_lossy);
static void _change_auto_flow_controlling_mode (GstMprtpscheduler * this,
    gboolean new_flow_controlling_mode);
static gboolean gst_mprtpscheduler_mprtp_src_event (GstPad * pad,
    GstObject * parent, GstEvent * event);
static GstStructure *_collect_infos (GstMprtpscheduler * this);
static void gst_mprtpscheduler_retain_process_run (void *data);
static gboolean _retain_buffer (GstMprtpscheduler * this, GstBuffer * buf);

static gboolean
_do_retain_buffer (GstMprtpscheduler * this,
    GstBuffer * buffer, MPRTPSPath ** selected);
static GstFlowReturn
_send_rtp_buffer (GstMprtpscheduler * this,
    MPRTPSPath * path, GstBuffer * buffer);
static gboolean
_retain_queue_try_push (GstMprtpscheduler * this, GstBuffer * result,
    GstClockTime time);
static gboolean _retain_queue_try_pull (GstMprtpscheduler * this,
    GstBuffer ** result);
static void _retain_queue_pop (GstMprtpscheduler * this);

static gboolean
_retain_queue_head (GstMprtpscheduler * this, GstBuffer ** result);


enum
{
  PROP_0,
  PROP_MPRTP_EXT_HEADER_ID,
  PROP_ABS_TIME_EXT_HEADER_ID,
  PROP_JOIN_SUBFLOW,
  PROP_DETACH_SUBFLOW,
  PROP_SET_SUBFLOW_NON_CONGESTED,
  PROP_SET_SUBFLOW_CONGESTED,
  PROP_AUTO_FLOW_CONTROLLING,
  PROP_SET_SENDING_BID,
  PROP_RETAIN_BUFFERS,
  PROP_SUBFLOWS_STATS,
  PROP_RETAIN_MAX_TIME_IN_MS,
  PROP_SET_MAX_BYTES_PER_MS,
  PROP_PACING_BUFFERS,
};

/* pad templates */

static GstStaticPadTemplate gst_mprtpscheduler_rtp_sink_template =
GST_STATIC_PAD_TEMPLATE ("rtp_sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp")
    );


static GstStaticPadTemplate gst_mprtpscheduler_mprtp_src_template =
GST_STATIC_PAD_TEMPLATE ("mprtp_src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-rtp")
    );


static GstStaticPadTemplate gst_mprtpscheduler_mprtcp_rr_sink_template =
GST_STATIC_PAD_TEMPLATE ("mprtcp_rr_sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
//    GST_STATIC_CAPS ("application/x-rtcp")
    GST_STATIC_CAPS_ANY);


static GstStaticPadTemplate gst_mprtpscheduler_mprtcp_sr_src_template =
GST_STATIC_PAD_TEMPLATE ("mprtcp_sr_src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
//    GST_STATIC_CAPS ("application/x-rtcp")
    GST_STATIC_CAPS_ANY);


/* class initialization */
G_DEFINE_TYPE_WITH_CODE (GstMprtpscheduler, gst_mprtpscheduler,
    GST_TYPE_ELEMENT,
    GST_DEBUG_CATEGORY_INIT (gst_mprtpscheduler_debug_category,
        "mprtpscheduler", 0, "debug category for mprtpscheduler element"));

#define GST_MPRTPSCHEDULER_GET_PRIVATE(obj)  \
    (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_MPRTPSCHEDULER, GstMprtpschedulerPrivate))

struct _GstMprtpschedulerPrivate
{
  gboolean have_same_caps;

  GstBufferPool *pool;
  gboolean pool_active;
  GstAllocator *allocator;
  GstAllocationParams params;
  GstQuery *query;
};

static void
gst_mprtpscheduler_class_init (GstMprtpschedulerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mprtpscheduler_rtp_sink_template));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mprtpscheduler_mprtp_src_template));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_mprtpscheduler_mprtcp_sr_src_template));

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get
      (&gst_mprtpscheduler_mprtcp_rr_sink_template));


  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "MPRTP Scheduler", "Generic", "MPRTP scheduler FIXME LONG DESC",
      "Balázs Kreith <balazs.kreith@gmail.com>");

  gobject_class->set_property = gst_mprtpscheduler_set_property;
  gobject_class->get_property = gst_mprtpscheduler_get_property;
  gobject_class->dispose = gst_mprtpscheduler_dispose;
  gobject_class->finalize = gst_mprtpscheduler_finalize;
  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_mprtpscheduler_change_state);
  element_class->query = GST_DEBUG_FUNCPTR (gst_mprtpscheduler_query);


  g_object_class_install_property (gobject_class, PROP_MPRTP_EXT_HEADER_ID,
      g_param_spec_uint ("mprtp-ext-header-id",
          "Set or get the id for the Multipath RTP extension",
          "Sets or gets the id for the extension header the MpRTP based on. The default is 3",
          0, 15, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));


  g_object_class_install_property (gobject_class, PROP_ABS_TIME_EXT_HEADER_ID,
      g_param_spec_uint ("abs-time-ext-header-id",
          "Set or get the id for the absolute time RTP extension",
          "Sets or gets the id for the extension header the absolute time based on. The default is 8",
          0, 15, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_JOIN_SUBFLOW,
      g_param_spec_uint ("join-subflow", "the subflow id requested to join",
          "Join a subflow with a given id.", 0,
          255, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DETACH_SUBFLOW,
      g_param_spec_uint ("detach-subflow", "the subflow id requested to detach",
          "Detach a subflow with a given id.", 0,
          255, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SET_SUBFLOW_CONGESTED,
      g_param_spec_uint ("congested-subflow", "set the subflow congested",
          "Set the subflow congested", 0,
          255, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_SET_SUBFLOW_NON_CONGESTED,
      g_param_spec_uint ("non-congested-subflow",
          "set the subflow non-congested", "Set the subflow non-congested", 0,
          255, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_AUTO_FLOW_CONTROLLING,
      g_param_spec_boolean ("auto-flow-controlling",
          "Automatic flow controlling means that ",
          "the scheduler makes decision"
          "based on receiver and XR riports for properly"
          "distribute the flow considering their capacities and "
          "events (losts, discards, etc...) It also puts extra "
          "bytes on the network because of the generated "
          "sender riports.",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_RETAIN_BUFFERS,
      g_param_spec_boolean ("retaining",
          "Indicate weather the scheduler retain buffers",
          "Indicate weather the schdeuler retain buffers if "
          "no active suflows or if the subflows are overused",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SET_SENDING_BID,
      g_param_spec_uint ("setup-sending-bid",
          "set the sending bid for a subflow",
          "A 32bit unsigned integer for setup a bid. The first 8 bit identifies the subflow, the latter the bid for the subflow",
          0, 4294967295, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SET_MAX_BYTES_PER_MS,
      g_param_spec_uint ("setup-max-byte-per-ms",
          "set the maximum allowed bytes per millisecond for pacing",
          "A 32bit unsigned integer for setup the value. The first 8 bit identifies the subflow, the latter the maximum allowed bytes per ms for the subflow",
          0, 4294967295, 0, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PACING_BUFFERS,
      g_param_spec_boolean ("pacing",
          "Indicate weather the scheduler pacing the traffic or not",
          "Indicate weather the scheduler pacing the traffic or not if"
          "suflows overuse the paths regarding to the goodputs",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));


  g_object_class_install_property (gobject_class, PROP_RETAIN_MAX_TIME_IN_MS,
      g_param_spec_uint ("retain-max-time",
          "WE DON'T CARE DIFFERENT TYPE OF QUEES HERE, I SUFFERED ENOUGH WITH OTHER THINGS, SO THIS PROPERTY IS GOING TO BE OBSOLATED."
          "Set the maximum time in ms the scheduler can retain a buffer (default: 30ms)",
          "If the scheduler can retain buffers for pacing or "
          "temporary no active subflow reasons, here you can set the "
          "maximum time for waiting",
          0, 500, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SUBFLOWS_STATS,
      g_param_spec_string ("subflow-stats",
          "Extract subflow stats",
          "Collect subflow statistics and return with "
          "a structure contains it",
          "NULL", G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}

static void
gst_mprtpscheduler_init (GstMprtpscheduler * this)
{
  this->rtp_sinkpad =
      gst_pad_new_from_static_template (&gst_mprtpscheduler_rtp_sink_template,
      "rtp_sink");


  gst_pad_set_chain_function (this->rtp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_mprtpscheduler_rtp_sink_chain));
  gst_pad_set_chain_list_function (this->rtp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_mprtpscheduler_rtp_sink_chainlist));

  GST_PAD_SET_PROXY_CAPS (this->rtp_sinkpad);
  GST_PAD_SET_PROXY_ALLOCATION (this->rtp_sinkpad);


  gst_element_add_pad (GST_ELEMENT (this), this->rtp_sinkpad);

  this->mprtcp_rr_sinkpad =
      gst_pad_new_from_static_template
      (&gst_mprtpscheduler_mprtcp_rr_sink_template, "mprtcp_rr_sink");

  gst_pad_set_chain_function (this->mprtcp_rr_sinkpad,
      GST_DEBUG_FUNCPTR (gst_mprtpscheduler_mprtcp_rr_sink_chain));

  gst_pad_set_query_function (this->rtp_sinkpad,
      GST_DEBUG_FUNCPTR (gst_mprtpscheduler_sink_query));

  gst_element_add_pad (GST_ELEMENT (this), this->mprtcp_rr_sinkpad);

  this->mprtcp_sr_srcpad =
      gst_pad_new_from_static_template
      (&gst_mprtpscheduler_mprtcp_sr_src_template, "mprtcp_sr_src");

  gst_element_add_pad (GST_ELEMENT (this), this->mprtcp_sr_srcpad);

  this->mprtp_srcpad =
      gst_pad_new_from_static_template (&gst_mprtpscheduler_mprtp_src_template,
      "mprtp_src");

  gst_pad_set_event_function (this->mprtp_srcpad,
      GST_DEBUG_FUNCPTR (gst_mprtpscheduler_mprtp_src_event));
  gst_pad_use_fixed_caps (this->mprtp_srcpad);
  GST_PAD_SET_PROXY_CAPS (this->mprtp_srcpad);
  GST_PAD_SET_PROXY_ALLOCATION (this->mprtp_srcpad);

  gst_element_add_pad (GST_ELEMENT (this), this->mprtp_srcpad);

  this->alpha_value = MPRTP_SENDER_DEFAULT_ALPHA_VALUE;
  this->beta_value = MPRTP_SENDER_DEFAULT_BETA_VALUE;
  this->gamma_value = MPRTP_SENDER_DEFAULT_GAMMA_VALUE;
  this->retain_allowed = TRUE;

  this->retained_thread =
      gst_task_new (gst_mprtpscheduler_retain_process_run, this, NULL);
  this->retained_queue_counter = 0;
  this->retained_process_started = FALSE;
  this->retained_queue_read_index = 0;
  this->retained_queue_write_index = 0;
  this->sysclock = gst_system_clock_obtain ();
  this->retain_max_time_in_ms = MPRTP_DEFAULT_MAX_RETAIN_TIME_IN_MS;
  g_rw_lock_init (&this->rwmutex);
  this->paths = g_hash_table_new_full (NULL, NULL, NULL, g_free);
  this->mprtp_ext_header_id = MPRTP_DEFAULT_EXTENSION_HEADER_ID;
  this->abs_time_ext_header_id = ABS_TIME_DEFAULT_EXTENSION_HEADER_ID;
  this->paths = g_hash_table_new_full (NULL, NULL, NULL, g_free);
  this->splitter = (StreamSplitter *) g_object_new (STREAM_SPLITTER_TYPE, NULL);
  this->controller = NULL;
  _change_auto_flow_controlling_mode (this, FALSE);
}


void
gst_mprtpscheduler_dispose (GObject * object)
{
  GstMprtpscheduler *mprtpscheduler = GST_MPRTPSCHEDULER (object);

  GST_DEBUG_OBJECT (mprtpscheduler, "dispose");

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (gst_mprtpscheduler_parent_class)->dispose (object);
}

void
gst_mprtpscheduler_finalize (GObject * object)
{
  GstMprtpscheduler *this = GST_MPRTPSCHEDULER (object);

  GST_DEBUG_OBJECT (this, "finalize");

  /* clean up object here */
  if (gst_task_get_state (this->retained_thread) == GST_TASK_STARTED) {
    gst_task_stop (this->retained_thread);
  }
  gst_task_join (this->retained_thread);
  gst_object_unref (this->retained_thread);

  g_object_unref (this->sysclock);
  G_OBJECT_CLASS (gst_mprtpscheduler_parent_class)->finalize (object);
}

void
gst_mprtpscheduler_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstMprtpscheduler *this = GST_MPRTPSCHEDULER (object);
  guint guint_value;
  gboolean gboolean_value;
  guint8 subflow_id;
  guint subflow_bid;
  guint max_bytes_per_ms;
  MPRTPSPath *path;

  GST_DEBUG_OBJECT (this, "set_property");

  switch (property_id) {
    case PROP_MPRTP_EXT_HEADER_ID:
      THIS_WRITELOCK (this);
      this->mprtp_ext_header_id = (guint8) g_value_get_uint (value);
      THIS_WRITEUNLOCK (this);
      break;
    case PROP_ABS_TIME_EXT_HEADER_ID:
      THIS_WRITELOCK (this);
      this->abs_time_ext_header_id = (guint8) g_value_get_uint (value);
      THIS_WRITEUNLOCK (this);
      break;
    case PROP_JOIN_SUBFLOW:
      THIS_WRITELOCK (this);
      _join_subflow (this, g_value_get_uint (value));
      THIS_WRITEUNLOCK (this);
      break;
    case PROP_DETACH_SUBFLOW:
      THIS_WRITELOCK (this);
      _detach_subflow (this, g_value_get_uint (value));
      THIS_WRITEUNLOCK (this);
      break;
    case PROP_SET_SUBFLOW_CONGESTED:
      THIS_WRITELOCK (this);
      guint_value = g_value_get_uint (value);
      _change_path_state (this, (guint8) guint_value, TRUE, FALSE);
      THIS_WRITEUNLOCK (this);
      break;
    case PROP_SET_SUBFLOW_NON_CONGESTED:
      THIS_WRITELOCK (this);
      guint_value = g_value_get_uint (value);
      _change_path_state (this, (guint8) guint_value, FALSE, FALSE);
      THIS_WRITEUNLOCK (this);
      break;
    case PROP_AUTO_FLOW_CONTROLLING:
      THIS_WRITELOCK (this);
      gboolean_value = g_value_get_boolean (value);
      _change_auto_flow_controlling_mode (this, gboolean_value);
      THIS_WRITEUNLOCK (this);
      break;
    case PROP_RETAIN_BUFFERS:
      THIS_WRITELOCK (this);
      gboolean_value = g_value_get_boolean (value);
      this->retain_allowed = gboolean_value;
      THIS_WRITEUNLOCK (this);
      break;
    case PROP_PACING_BUFFERS:
      THIS_WRITELOCK (this);
      gboolean_value = g_value_get_boolean (value);
      this->controller_pacing (this->controller, gboolean_value);
      THIS_WRITEUNLOCK (this);
      break;
    case PROP_RETAIN_MAX_TIME_IN_MS:
      THIS_WRITELOCK (this);
      this->retain_max_time_in_ms = g_value_get_uint (value);
      THIS_WRITEUNLOCK (this);
      break;
    case PROP_SET_SENDING_BID:
      THIS_WRITELOCK (this);
      guint_value = g_value_get_uint (value);
      subflow_id = (guint8) ((guint_value >> 24) & 0x000000FF);
      subflow_bid = guint_value & 0x00FFFFFFUL;
      stream_splitter_setup_sending_bid (this->splitter, subflow_id,
          subflow_bid);
      stream_splitter_commit_changes (this->splitter);
      THIS_WRITEUNLOCK (this);
      break;

    case PROP_SET_MAX_BYTES_PER_MS:
      THIS_WRITELOCK (this);
      guint_value = g_value_get_uint (value);
      subflow_id = (guint8) ((guint_value >> 24) & 0x000000FF);
      path = NULL;
      _try_get_path (this, subflow_id, &path);
      max_bytes_per_ms = guint_value & 0x00FFFFFFUL;
      mprtps_path_set_max_bytes_per_ms (path, max_bytes_per_ms);
      THIS_WRITEUNLOCK (this);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


void
gst_mprtpscheduler_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstMprtpscheduler *this = GST_MPRTPSCHEDULER (object);

  GST_DEBUG_OBJECT (this, "get_property");

  switch (property_id) {
    case PROP_MPRTP_EXT_HEADER_ID:
      THIS_READLOCK (this);
      g_value_set_uint (value, (guint) this->mprtp_ext_header_id);
      THIS_READUNLOCK (this);
      break;
    case PROP_ABS_TIME_EXT_HEADER_ID:
      THIS_READLOCK (this);
      g_value_set_uint (value, (guint) this->abs_time_ext_header_id);
      THIS_READUNLOCK (this);
      break;
    case PROP_RETAIN_MAX_TIME_IN_MS:
      THIS_READLOCK (this);
      g_value_set_uint (value, (guint) this->retain_max_time_in_ms);
      THIS_READUNLOCK (this);
      break;
    case PROP_AUTO_FLOW_CONTROLLING:
      THIS_READLOCK (this);
      g_value_set_boolean (value, this->flow_controlling_mode);
      THIS_READUNLOCK (this);
      break;
    case PROP_RETAIN_BUFFERS:
      THIS_READLOCK (this);
      g_value_set_boolean (value, this->retain_allowed);
      THIS_READUNLOCK (this);
      break;
    case PROP_SUBFLOWS_STATS:
      THIS_READLOCK (this);
      g_value_set_string (value,
          gst_structure_to_string (_collect_infos (this)));
      THIS_READUNLOCK (this);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}


GstStructure *
_collect_infos (GstMprtpscheduler * this)
{
  GstStructure *result;
  GHashTableIter iter;
  gpointer key, val;
  MPRTPSPath *path;
  gint index = 0;
  GValue g_value = { 0 };
  gchar *field_name;
  result = gst_structure_new ("SchedulerSubflowReports",
      "length", G_TYPE_UINT, this->subflows_num, NULL);
  g_value_init (&g_value, G_TYPE_UINT);
  g_hash_table_iter_init (&iter, this->paths);
  while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & val)) {
    path = (MPRTPSPath *) val;

    field_name = g_strdup_printf ("subflow-%d-id", index);
    g_value_set_uint (&g_value, mprtps_path_get_id (path));
    gst_structure_set_value (result, field_name, &g_value);
    g_free (field_name);

    field_name = g_strdup_printf ("subflow-%d-sent_packet_num", index);
    g_value_set_uint (&g_value, mprtps_path_get_total_sent_packets_num (path));
    gst_structure_set_value (result, field_name, &g_value);
    g_free (field_name);

    field_name = g_strdup_printf ("subflow-%d-sent_payload_bytes", index);
    g_value_set_uint (&g_value,
        mprtps_path_get_total_sent_payload_bytes (path));
    gst_structure_set_value (result, field_name, &g_value);
    g_free (field_name);

    ++index;
  }
  return result;
}

gboolean
gst_mprtpscheduler_mprtp_src_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstMprtpscheduler *this;
  gboolean result;

  this = GST_MPRTPSCHEDULER (parent);
  THIS_READLOCK (this);
  switch (GST_EVENT_TYPE (event)) {
    default:
      result = gst_pad_event_default (pad, parent, event);
  }
  THIS_READUNLOCK (this);
  return result;
}

void
_join_subflow (GstMprtpscheduler * this, guint subflow_id)
{
  MPRTPSPath *path;
  path =
      (MPRTPSPath *) g_hash_table_lookup (this->paths,
      GINT_TO_POINTER (subflow_id));
  if (path != NULL) {
    GST_WARNING_OBJECT (this, "Join operation can not be done "
        "due to duplicated subflow id (%d)", subflow_id);
    return;
  }
  path = make_mprtps_path ((guint8) subflow_id);
  g_hash_table_insert (this->paths, GINT_TO_POINTER (subflow_id),
      make_mprtps_path (subflow_id));
  stream_splitter_add_path (this->splitter, subflow_id, path);
  this->controller_add_path (this->controller, subflow_id, path);
  ++this->subflows_num;
}

void
_detach_subflow (GstMprtpscheduler * this, guint subflow_id)
{
  MPRTPSPath *path;

  path =
      (MPRTPSPath *) g_hash_table_lookup (this->paths,
      GINT_TO_POINTER (subflow_id));
  if (path == NULL) {
    GST_WARNING_OBJECT (this, "Detach operation can not be done "
        "due to not existed subflow id (%d)", subflow_id);
    return;
  }
  g_hash_table_remove (this->paths, GINT_TO_POINTER (subflow_id));
  stream_splitter_rem_path (this->splitter, subflow_id);
  this->controller_rem_path (this->controller, subflow_id);
  --this->subflows_num;
}



static GstStateChangeReturn
gst_mprtpscheduler_change_state (GstElement * element,
    GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstMprtpscheduler *this;

  g_return_val_if_fail (GST_IS_MPRTPSCHEDULER (element),
      GST_STATE_CHANGE_FAILURE);
  this = GST_MPRTPSCHEDULER (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      gst_task_set_lock (this->retained_thread, &this->retained_thread_mutex);
      gst_task_pause (this->retained_thread);
      break;
    default:
      break;
  }

  ret =
      GST_ELEMENT_CLASS (gst_mprtpscheduler_parent_class)->change_state
      (element, transition);

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
gst_mprtpscheduler_query (GstElement * element, GstQuery * query)
{
  GstMprtpscheduler *this = GST_MPRTPSCHEDULER (element);
  gboolean ret = TRUE;
  GstStructure *s = NULL;

  GST_DEBUG_OBJECT (this, "query");
  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CUSTOM:
      THIS_READLOCK (this);
      s = gst_query_writable_structure (query);
      if (!gst_structure_has_name (s,
              GST_MPRTCP_SCHEDULER_SENT_BYTES_STRUCTURE_NAME)) {
        ret =
            GST_ELEMENT_CLASS (gst_mprtpscheduler_parent_class)->query (element,
            query);
        break;
      }
      gst_structure_set (s,
          GST_MPRTCP_SCHEDULER_SENT_OCTET_SUM_FIELD,
          G_TYPE_UINT, this->rtcp_sent_octet_sum, NULL);
      THIS_READUNLOCK (this);
      ret = TRUE;
      break;
    default:
      ret =
          GST_ELEMENT_CLASS (gst_mprtpscheduler_parent_class)->query (element,
          query);
      break;
  }

  return ret;
}

static GstFlowReturn
gst_mprtpscheduler_rtp_sink_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buffer)
{
  GstMprtpscheduler *this;
  MPRTPSPath *path;
  GstFlowReturn result;
  guint8 first_byte;
  GstClockTime now;
  result = GST_FLOW_OK;
  this = GST_MPRTPSCHEDULER (parent);
  if (gst_buffer_extract (buffer, 0, &first_byte, 1) != 1) {
    GST_WARNING_OBJECT (this, "could not extract first byte from buffer");
    gst_buffer_unref (buffer);
    return GST_FLOW_OK;
  }

  if (!PACKET_IS_RTP_OR_RTCP (first_byte)) {
    GST_WARNING_OBJECT (this, "Not RTP Packet arrived at rtp_sink");
    return gst_pad_push (this->mprtp_srcpad, buffer);
  }
  //the packet is rtp

  now = gst_clock_get_time (this->sysclock);

  THIS_READLOCK (this);
  if (_do_retain_buffer (this, buffer, &path)) {
    goto retain_and_done;
  }
  if (this->retained_queue_counter > 0) {
    if (!_retain_queue_try_push (this, buffer, now)) {
      GST_WARNING_OBJECT (this, "Retain buffer is full");
      result = GST_FLOW_CUSTOM_ERROR;
      goto done;
    }
    gst_buffer_ref (buffer);
    while (_retain_queue_try_pull (this, &buffer)) {
      result = _send_rtp_buffer (this, path, buffer);
      if (result != GST_FLOW_OK) {
        goto done;
      }
      if (_do_retain_buffer (this, buffer, &path)) {
        goto retain_and_done;
      }
    }
    goto done;
  }

  if (this->retained_process_started &&
      this->retained_last_popped_item_time < now - GST_SECOND) {
    GST_DEBUG_OBJECT (this, "The reatining process is stopped");
    gst_task_pause (this->retained_thread);
    this->retained_process_started = FALSE;
  }

  result = _send_rtp_buffer (this, path, buffer);
  goto done;

retain_and_done:
  if (!this->retain_allowed) {
    result = _send_rtp_buffer (this, path, buffer);
    goto done;
  }
  THIS_READUNLOCK (this);
  THIS_WRITELOCK (this);
  if (!_retain_buffer (this, buffer)) {
    GST_WARNING_OBJECT (this, "Can not retain buffer");
    result = GST_FLOW_CUSTOM_ERROR;
  } else {
    gst_buffer_ref (buffer);
  }
  THIS_WRITEUNLOCK (this);
  return result;
done:
  THIS_READUNLOCK (this);
  return result;
}

gboolean
_do_retain_buffer (GstMprtpscheduler * this,
    GstBuffer * buffer, MPRTPSPath ** selected)
{
  *selected = stream_splitter_get_next_path (this->splitter, buffer);
  if (*selected == NULL) {
    GST_WARNING_OBJECT (this, "No active subflow");
    return TRUE;
  }
  if (mprtps_path_is_overused (*selected)) {
    GST_WARNING_OBJECT (this, "Paths are overused");
    return TRUE;
  }

  return FALSE;
}

GstFlowReturn
_send_rtp_buffer (GstMprtpscheduler * this,
    MPRTPSPath * path, GstBuffer * buffer)
{
  GstFlowReturn result = GST_FLOW_OK;
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  GstBuffer *outbuf;
  RTPAbsTimeExtension data;
  GstClockTime now;
  guint32 time;

  outbuf = gst_buffer_make_writable (buffer);
  if (G_UNLIKELY (!gst_rtp_buffer_map (outbuf, GST_MAP_READWRITE, &rtp))) {
    GST_WARNING_OBJECT (this, "The RTP packet is not writeable");
    goto done;
  }
  mprtps_path_process_rtp_packet (path, this->mprtp_ext_header_id, &rtp);

  //Absolute sending time
  now = gst_clock_get_time (this->sysclock);
  //https://tools.ietf.org/html/draft-alvestrand-rmcat-remb-03
  time = (guint32) (now >> 14) & 0x00ffffff;
  memcpy (&data, &time, 3);
  gst_rtp_buffer_add_extension_onebyte_header (&rtp,
      this->abs_time_ext_header_id, (gpointer) & data, sizeof (data));
  gst_rtp_buffer_unmap (&rtp);

  result = gst_pad_push (this->mprtp_srcpad, outbuf);
  if (!this->riport_flow_signal_sent) {
    this->riport_flow_signal_sent = TRUE;
    this->riport_can_flow (this->controller);
  }
done:
  return result;
}

static GstFlowReturn
gst_mprtpscheduler_rtp_sink_chainlist (GstPad * pad, GstObject * parent,
    GstBufferList * list)
{
  GstBuffer *buffer;
  gint i, len;
  GstFlowReturn result;

  result = GST_FLOW_OK;

  /* chain each buffer in list individually */
  len = gst_buffer_list_length (list);

  if (len == 0)
    goto done;

  for (i = 0; i < len; i++) {
    buffer = gst_buffer_list_get (list, i);

    result = gst_mprtpscheduler_rtp_sink_chain (pad, parent, buffer);
    if (result != GST_FLOW_OK)
      break;
  }

done:
  return result;
}


static GstFlowReturn
gst_mprtpscheduler_mprtcp_rr_sink_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buf)
{
  GstMprtpscheduler *this;
  GstFlowReturn result;

  this = GST_MPRTPSCHEDULER (parent);
  GST_DEBUG_OBJECT (this, "RTCP/MPRTCP sink");
  THIS_READLOCK (this);

  this->mprtcp_receiver (this->controller, buf);

  result = GST_FLOW_OK;
  THIS_READUNLOCK (this);
  return result;

}

void
gst_mprtpscheduler_mprtcp_sender (gpointer ptr, GstBuffer * buf)
{
  GstMprtpscheduler *this;
  this = GST_MPRTPSCHEDULER (ptr);
  THIS_READLOCK (this);
  gst_pad_push (this->mprtcp_sr_srcpad, buf);
  THIS_READUNLOCK (this);
}

static gboolean
gst_mprtpscheduler_sink_query (GstPad * sinkpad, GstObject * parent,
    GstQuery * query)
{
  GstMprtpscheduler *this = GST_MPRTPSCHEDULER (parent);
  gboolean result;
  GST_DEBUG_OBJECT (this, "query");
  switch (GST_QUERY_TYPE (query)) {

    default:
      result = gst_pad_peer_query (this->mprtp_srcpad, query);
      break;
  }

  return result;
}


void
gst_mprtpscheduler_retain_process_run (void *data)
{
  GstMprtpscheduler *this;
  GstClockID clock_id;
  GstClockTime next_scheduler_time;
  GstBuffer *buf;
  MPRTPSPath *path;
  GstClockTime now;
  this = (GstMprtpscheduler *) data;

  THIS_WRITELOCK (this);
  now = gst_clock_get_time (this->sysclock);
  while (_retain_queue_head (this, &buf)) {
    if (_do_retain_buffer (this, buf, &path)) {
      goto done;
    }
    _retain_queue_pop (this);
    if (_send_rtp_buffer (this, path, buf) != GST_FLOW_OK) {
      GST_WARNING_OBJECT (this, "Sending mprtp buffer was not successful");
      goto done;
    }
  }
done:
  next_scheduler_time = now + 10 * GST_MSECOND;
  clock_id = gst_clock_new_single_shot_id (this->sysclock, next_scheduler_time);
  THIS_WRITEUNLOCK (this);

  if (gst_clock_id_wait (clock_id, NULL) == GST_CLOCK_UNSCHEDULED) {
    GST_WARNING_OBJECT (this, "The scheduler clock wait is interrupted");
  }
  gst_clock_id_unref (clock_id);
}

gboolean
_retain_buffer (GstMprtpscheduler * this, GstBuffer * buf)
{
  gboolean result;

  result =
      _retain_queue_try_push (this, buf, gst_clock_get_time (this->sysclock));
  if (result == FALSE) {
    return FALSE;
  }
  if (gst_task_get_state (this->retained_thread) != GST_TASK_STARTED) {
    result &= gst_task_start (this->retained_thread);
    this->retained_process_started = TRUE;
  }
  return result;
}


//ref the buffer!
gboolean
_retain_queue_try_push (GstMprtpscheduler * this, GstBuffer * buf,
    GstClockTime time)
{
  if (this->retained_queue_counter == SCHEDULER_RETAIN_QUEUE_MAX_ITEMS) {
    return FALSE;
  }
  this->retained_queue[this->retained_queue_write_index].time = time;
  this->retained_queue[this->retained_queue_write_index].buffer = buf;
  this->retained_queue_write_index += 1;
  this->retained_queue_write_index &= SCHEDULER_RETAIN_QUEUE_MAX_ITEMS;
  ++this->retained_queue_counter;

  return TRUE;
}


gboolean
_retain_queue_try_pull (GstMprtpscheduler * this, GstBuffer ** buffer)
{
  if (!_retain_queue_head (this, buffer)) {
    return FALSE;
  }
  _retain_queue_pop (this);
  return TRUE;
}


gboolean
_retain_queue_head (GstMprtpscheduler * this, GstBuffer ** result)
{
  GstBuffer *buffer;
  if (this->retained_queue_counter == 0) {
    return FALSE;
  }

  buffer = this->retained_queue[this->retained_queue_read_index].buffer;

  *result = buffer;
  return TRUE;
}



void
_retain_queue_pop (GstMprtpscheduler * this)
{
  GstClockTime now;
  now = gst_clock_get_time (this->sysclock);
  if (this->retained_queue_counter == 0) {
    return;
  }
  this->retained_last_popped_item_time = now;
  this->retained_queue_read_index += 1;
  this->retained_queue_read_index &= SCHEDULER_RETAIN_QUEUE_MAX_ITEMS;
  --this->retained_queue_counter;

}




gboolean
_try_get_path (GstMprtpscheduler * this, guint16 subflow_id,
    MPRTPSPath ** result)
{
  MPRTPSPath *path;
  path =
      (MPRTPSPath *) g_hash_table_lookup (this->paths,
      GINT_TO_POINTER (subflow_id));
  if (path == NULL) {
    return FALSE;
  }
  if (result) {
    *result = path;
  }
  return TRUE;
}


void
_change_auto_flow_controlling_mode (GstMprtpscheduler * this,
    gboolean new_flow_controlling_mode)
{
  gpointer key, val;
  MPRTPSPath *path;
  GHashTableIter iter;
  guint8 subflow_id;
  if (this->controller
      && this->flow_controlling_mode == new_flow_controlling_mode) {
    return;
  }
  if (this->controller != NULL) {
    g_object_unref (this->controller);
  }
  if (new_flow_controlling_mode) {
    this->controller = g_object_new (SEFCTRLER_TYPE, NULL);
    sefctrler_setup (this->controller, this->splitter);
    this->mprtcp_receiver = sefctrler_setup_mprtcp_exchange (this->controller,
        this, gst_mprtpscheduler_mprtcp_sender);

    sefctrler_set_callbacks (&this->riport_can_flow,
        &this->controller_add_path,
        &this->controller_rem_path, &this->controller_pacing);
  } else {
    this->controller = g_object_new (SMANCTRLER_TYPE, NULL);
    this->mprtcp_receiver = smanctrler_setup_mprtcp_exchange (this->controller,
        this, gst_mprtpscheduler_mprtcp_sender);

    smanctrler_set_callbacks (&this->riport_can_flow,
        &this->controller_add_path,
        &this->controller_rem_path, &this->controller_pacing);
  }
  this->riport_flow_signal_sent = FALSE;
  g_hash_table_iter_init (&iter, this->paths);
  while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & val)) {
    path = (MPRTPSPath *) val;
    subflow_id = (guint8) GPOINTER_TO_INT (key);
    this->controller_add_path (this->controller, subflow_id, path);
  }

}


void
_change_path_state (GstMprtpscheduler * this, guint8 subflow_id,
    gboolean set_congested, gboolean set_lossy)
{
  MPRTPSPath *path;
  if (!_try_get_path (this, subflow_id, &path)) {
    GST_WARNING_OBJECT (this,
        "Change state can not be done due to unknown subflow id (%d)",
        subflow_id);
    return;
  }

  if (!set_congested) {
    mprtps_path_set_non_congested (path);
  } else {
    mprtps_path_set_congested (path);
  }

  if (!set_lossy) {
    mprtps_path_set_non_lossy (path);
  } else {
    mprtps_path_set_lossy (path);
  }

}

#undef THIS_WRITELOCK
#undef THIS_WRITEUNLOCK
#undef THIS_READLOCK
#undef THIS_READUNLOCK
#undef MPRTP_SENDER_DEFAULT_CHARGE_VALUE
#undef MPRTP_SENDER_DEFAULT_ALPHA_VALUE
#undef MPRTP_SENDER_DEFAULT_BETA_VALUE
#undef MPRTP_SENDER_DEFAULT_GAMMA_VALUE
#undef PACKET_IS_RTP_OR_RTCP
#undef MPRTP_DEFAULT_MAX_RETAIN_TIME_IN_MS
