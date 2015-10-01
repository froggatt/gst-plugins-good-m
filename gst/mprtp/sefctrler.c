/* GStreamer Scheduling tree
 * Copyright (C) 2015 Balázs Kreith (contact: balazs.kreith@gmail.com)
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include "sefctrler.h"
#include "streamsplitter.h"
#include "gstmprtcpbuffer.h"
#include <math.h>
#include <string.h>

#define THIS_READLOCK(this) g_rw_lock_reader_lock(&this->rwmutex)
#define THIS_READUNLOCK(this) g_rw_lock_reader_unlock(&this->rwmutex)
#define THIS_WRITELOCK(this) g_rw_lock_writer_lock(&this->rwmutex)
#define THIS_WRITEUNLOCK(this) g_rw_lock_writer_unlock(&this->rwmutex)

#define MAX_RIPORT_INTERVAL (5 * GST_SECOND)
#define RIPORT_TIMEOUT (3 * MAX_RIPORT_INTERVAL)
#define PATH_RTT_MAX_TRESHOLD (400 * GST_MSECOND)
#define PATH_RTT_MIN_TRESHOLD (200 * GST_MSECOND)
GST_DEBUG_CATEGORY_STATIC (sefctrler_debug_category);
#define GST_CAT_DEFAULT sefctrler_debug_category

G_DEFINE_TYPE (SndEventBasedController, sefctrler, G_TYPE_OBJECT);

typedef struct _Subflow Subflow;
typedef enum
{
  EVENT_FI,
  EVENT_DISTORTION,
  EVENT_SETTLED,
  EVENT_CONGESTION,
  EVENT_LATE,
  EVENT_TRY,
  EVENT_LOSSY,
} Event;

struct _Subflow
{
  MPRTPSPath *path;
  guint8 id;
  guint16 HSSN;
  guint16 HSSN_diff;
  GstClock *sysclock;

  GstClockTime riport_interval_time;
  GstClockTime next_riport_time;
  gboolean riport_check_started;
  gboolean first_reicever_riport_arrived;
  guint consecutive_lost;
  guint consecutive_non_lost;
  guint consecutive_discarded;
  guint consecutive_non_discarded;
  gfloat lost_rate;
  GstClockTime joined_time;
  GstClockTime RTT;
  GstClockTime last_receiver_riport_time;
  GstClockTime last_xr_rfc7243_riport_time;
  GstClockTime last_sender_riport_time;
  gfloat goodput;
  guint32 media_rate;
  gfloat sr_riport_bw;
  gdouble avg_rtcp_size;


  guint32 late_discarded_bytes;
  guint32 late_discarded_bytes_sum;
  guint32 early_discarded_bytes;
  guint32 early_discarded_bytes_sum;

  struct
  {
    gfloat increasement;
    gfloat decreasement;
  } for_bid_calc;

  guint32 last_packet_count_for_sr;
  guint32 last_packet_count_for_goodput;
  guint32 actual_packet_count;
  guint32 last_payload_bytes_for_sr;
  guint32 last_payload_bytes_for_goodput;
  guint32 actual_sent_payload_bytes;

};

//----------------------------------------------------------------------
//-------- Private functions belongs to Scheduler tree object ----------
//----------------------------------------------------------------------

static void sefctrler_finalize (GObject * object);
static void sefctrler_run (void *data);

static guint16 _uint16_diff (guint16 a, guint16 b);
static guint32 _uint32_diff (guint32 a, guint32 b);
static void sefctrler_receive_mprtcp (gpointer this, GstBuffer * buf);
static void _report_processing_selector (Subflow * this,
    GstMPRTCPSubflowBlock * block);
static void
_riport_processing_rrblock_processor (Subflow * this, GstRTCPRRBlock * rrb);
static void
_riport_processing_xrblock_processor (Subflow * this, GstRTCPXR_RFC7243 * xrb);

Event _check_state (Subflow * this);
static void _setup_sr_riport (Subflow * this, GstRTCPSR * sr, guint32 ssrc);
static void _fire (SndEventBasedController * this, Subflow * subflow,
    Event event);
static void _recalc (SndEventBasedController * this);
static gfloat _get_goodput (Subflow * this);
static void _validate_cross_reports_data (Subflow * this);
static gfloat _get_alpha (Subflow * this);
static gfloat _get_beta (Subflow * this);
static gfloat _get_gamma (Subflow * this);

static Event _check_riport_timeout (Subflow * this);
static void sefctrler_riport_can_flow (gpointer this);
//subflow functions
static Subflow *make_subflow (guint8 id, MPRTPSPath * path);
static void ruin_subflow (gpointer * subflow);
static void reset_subflow (Subflow * this);
static Subflow *subflow_ctor (void);
static void subflow_dtor (Subflow * this);

static void sefctrler_rem_path (gpointer controller_ptr, guint8 subflow_id);
static void sefctrler_add_path (gpointer controller_ptr, guint8 subflow_id,
    MPRTPSPath * path);

//----------------------------------------------------------------------
//--------- Private functions implementations to SchTree object --------
//----------------------------------------------------------------------


void
sefctrler_class_init (SndEventBasedControllerClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = sefctrler_finalize;

  GST_DEBUG_CATEGORY_INIT (sefctrler_debug_category, "sefctrler", 0,
      "MpRTP Sending Event Based Flow Controller");

}

void
sefctrler_finalize (GObject * object)
{
  SndEventBasedController *this = SEFCTRLER (object);
  g_hash_table_destroy (this->subflows);
  gst_task_stop (this->thread);
  gst_task_join (this->thread);

  g_object_unref (this->sysclock);
}

void
sefctrler_init (SndEventBasedController * this)
{
  this->sysclock = gst_system_clock_obtain ();
  this->subflows = g_hash_table_new_full (NULL, NULL,
      NULL, (GDestroyNotify) ruin_subflow);
  this->subflow_num = 0;
  this->bids_recalc_requested = FALSE;
  this->bids_commit_requested = FALSE;
  this->ssrc = g_random_int ();
  this->riport_is_flowable = FALSE;

  g_rw_lock_init (&this->rwmutex);
  g_rec_mutex_init (&this->thread_mutex);
  this->thread = gst_task_new (sefctrler_run, this, NULL);
  gst_task_set_lock (this->thread, &this->thread_mutex);
  gst_task_start (this->thread);
}

void
sefctrler_run (void *data)
{
  GstClockTime now, next_scheduler_time;
  SndEventBasedController *this;
  GHashTableIter iter;
  gpointer key, val;
  Subflow *subflow;
  Event event;
  MPRTPSPathState path_state;
  gfloat goodput_nc_sum = 0.;
  gfloat goodput_c_sum = 0.;
  gfloat goodput_mc_sum = 0.;
  MPRTPSPath *path;
  GstBuffer *buf;
  GstClockID clock_id;
  gpointer dataptr;
  GstMPRTCPSubflowBlock block;
  GstRTCPSR *sr;
  guint8 block_length;
  guint16 length;

  this = SEFCTRLER (data);
  THIS_WRITELOCK (this);
  now = gst_clock_get_time (this->sysclock);

  g_hash_table_iter_init (&iter, this->subflows);
  while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & val)) {
    subflow = (Subflow *) val;
    path = subflow->path;

    if ((event = _check_riport_timeout (subflow)) == EVENT_LATE) {
      _fire (this, subflow, event);
      continue;
    }

    if (this->riport_is_flowable) {
      gst_mprtcp_block_init (&block);
      sr = gst_mprtcp_riport_block_add_sr (&block);
      _setup_sr_riport (subflow, sr, this->ssrc);
      gst_rtcp_header_getdown (&sr->header, NULL, NULL, NULL, NULL, &length,
          NULL);
      block_length = (guint8) length + 1;
      gst_mprtcp_block_setup (&block.info, MPRTCP_BLOCK_TYPE_RIPORT,
          block_length, (guint16) subflow->id);
      length = (block_length + 1) << 2;
      dataptr = g_malloc0 (length);
      memcpy (dataptr, &block, length);
      buf = gst_buffer_new_wrapped (dataptr, length);
      this->send_mprtcp_packet_func (this->send_mprtcp_packet_data, buf);
    }

    path_state = mprtps_path_get_state (path);
    switch (path_state) {
      case MPRTPS_PATH_STATE_NON_CONGESTED:
        goodput_nc_sum += subflow->goodput;
        break;
      case MPRTPS_PATH_STATE_MIDDLY_CONGESTED:
        goodput_mc_sum += subflow->goodput;
        break;
      case MPRTPS_PATH_STATE_CONGESTED:
        goodput_c_sum += subflow->goodput;
        break;
      default:
        break;
    }
  }
  this->goodput_nc_sum = goodput_nc_sum;
  this->goodput_c_sum = goodput_c_sum;
  this->goodput_mc_sum = goodput_mc_sum;
  if (this->bids_recalc_requested) {
    this->bids_recalc_requested = FALSE;
    this->bids_commit_requested = TRUE;
    _recalc (this);
  }
  if (this->bids_commit_requested) {
    this->bids_commit_requested = FALSE;
    stream_splitter_commit_changes (this->splitter);
  }
//done:
  next_scheduler_time = now + 100 * GST_MSECOND;
  THIS_WRITEUNLOCK (this);
  clock_id = gst_clock_new_single_shot_id (this->sysclock, next_scheduler_time);

  if (gst_clock_id_wait (clock_id, NULL) == GST_CLOCK_UNSCHEDULED) {
    GST_WARNING_OBJECT (this, "The playout clock wait is interrupted");
  }
  gst_clock_id_unref (clock_id);
  //clockshot;
}


void
sefctrler_add_path (gpointer ptr, guint8 subflow_id, MPRTPSPath * path)
{
  Subflow *lookup_result;
  SndEventBasedController *this;
  this = SEFCTRLER (ptr);
  THIS_WRITELOCK (this);
  lookup_result =
      (Subflow *) g_hash_table_lookup (this->subflows,
      GINT_TO_POINTER (subflow_id));
  if (lookup_result != NULL) {
    GST_WARNING_OBJECT (this, "The requested add operation can not be done "
        "due to duplicated subflow id (%d)", subflow_id);
    goto exit;
  }
  g_hash_table_insert (this->subflows, GINT_TO_POINTER (subflow_id),
      make_subflow (subflow_id, path));
  ++this->subflow_num;
exit:
  THIS_WRITEUNLOCK (this);
}

void
sefctrler_rem_path (gpointer ptr, guint8 subflow_id)
{
  Subflow *lookup_result;
  SndEventBasedController *this;
  this = SEFCTRLER (ptr);
  THIS_WRITELOCK (this);
  lookup_result =
      (Subflow *) g_hash_table_lookup (this->subflows,
      GINT_TO_POINTER (subflow_id));
  if (lookup_result == NULL) {
    GST_WARNING_OBJECT (this, "The requested remove operation can not be done "
        "due to not existed subflow id (%d)", subflow_id);
    goto exit;
  }
  g_hash_table_remove (this->subflows, GINT_TO_POINTER (subflow_id));
  if (--this->subflow_num < 0) {
    this->subflow_num = 0;
  }
exit:
  THIS_WRITEUNLOCK (this);
}

void
sefctrler_set_callbacks (void (**riport_can_flow_indicator) (gpointer),
    void (**controller_add_path) (gpointer, guint8, MPRTPSPath *),
    void (**controller_rem_path) (gpointer, guint8))
{
  if (riport_can_flow_indicator) {
    *riport_can_flow_indicator = sefctrler_riport_can_flow;
  }
  if (controller_add_path) {
    *controller_add_path = sefctrler_add_path;
  }
  if (controller_rem_path) {
    *controller_rem_path = sefctrler_rem_path;
  }
}

void
sefctrler_setup (SndEventBasedController * this, StreamSplitter * splitter)
{
  THIS_WRITELOCK (this);
  this->splitter = splitter;
  THIS_WRITEUNLOCK (this);
}

GstBufferReceiverFunc
sefctrler_setup_mprtcp_exchange (SndEventBasedController * this,
    gpointer data, GstBufferReceiverFunc func)
{
  GstBufferReceiverFunc result;
  THIS_WRITELOCK (this);
  this->send_mprtcp_packet_func = func;
  this->send_mprtcp_packet_data = data;
  result = sefctrler_receive_mprtcp;
  THIS_WRITEUNLOCK (this);
  return result;
}


void
sefctrler_receive_mprtcp (gpointer ptr, GstBuffer * buf)
{
  GstMPRTCPSubflowBlock *block;
  SndEventBasedController *this = SEFCTRLER (ptr);
  guint16 subflow_id;
  guint8 info_type;
  Subflow *subflow;
  GstMapInfo map = GST_MAP_INFO_INIT;
  Event event;
  if (G_UNLIKELY (!gst_buffer_map (buf, &map, GST_MAP_READ))) {
    GST_WARNING_OBJECT (this, "The buffer is not readable");
    return;
  }
  block = (GstMPRTCPSubflowBlock *) map.data;
  THIS_WRITELOCK (this);

  gst_mprtcp_block_getdown (&block->info, &info_type, NULL, &subflow_id);
  if (info_type != MPRTCP_BLOCK_TYPE_RIPORT) {
    goto done;
  }
  subflow =
      (Subflow *) g_hash_table_lookup (this->subflows,
      GINT_TO_POINTER (subflow_id));

  if (subflow == NULL) {
    GST_WARNING_OBJECT (this,
        "MPRTCP riport can not be binded any "
        "subflow with the given id: %d", subflow_id);
    goto done;
  }
  _report_processing_selector (subflow, block);

  //validate and fire.
  _validate_cross_reports_data (subflow);
  event = _check_state (subflow);
  g_print ("Subflow: %d Actual state: %d event: %d\n",
      subflow->id, mprtps_path_get_state (subflow->path), event);
  if (event != EVENT_FI) {
    _fire (this, subflow, event);
  }

done:
  gst_buffer_unmap (buf, &map);
  THIS_WRITEUNLOCK (this);
}

void
sefctrler_riport_can_flow (gpointer ptr)
{
  SndEventBasedController *this;
  this = SEFCTRLER (ptr);
  GST_DEBUG_OBJECT (this, "RTCP riport can now flowable");
  THIS_WRITELOCK (this);
  this->riport_is_flowable = TRUE;
  THIS_WRITEUNLOCK (this);
}


//----------------------------------------------
// -------- Subflow related functions ----------
//----------------------------------------------
Subflow *
subflow_ctor (void)
{
  Subflow *result;
  result = g_malloc0 (sizeof (Subflow));
  return result;
}

void
subflow_dtor (Subflow * this)
{
  g_return_if_fail (this);
  g_free (this);
}

void
ruin_subflow (gpointer * subflow)
{
  Subflow *this;
  g_return_if_fail (subflow);
  this = (Subflow *) subflow;
  g_object_unref (this->sysclock);
  g_object_unref (this->path);
  subflow_dtor (this);
}

Subflow *
make_subflow (guint8 id, MPRTPSPath * path)
{
  Subflow *result = subflow_ctor ();
  g_object_ref (path);
  result->sysclock = gst_system_clock_obtain ();
  result->path = path;
  result->id = id;
  result->joined_time = gst_clock_get_time (result->sysclock);
  reset_subflow (result);
  return result;
}

void
reset_subflow (Subflow * this)
{
  this->HSSN = 0;
  this->riport_check_started = FALSE;
  this->riport_interval_time = 0;
  this->next_riport_time = 0;
  this->riport_check_started = FALSE;
  this->first_reicever_riport_arrived = FALSE;
  this->consecutive_lost = 0;
  this->consecutive_non_lost = 0;
  this->lost_rate = 0.;
  this->RTT = 0;
  this->last_receiver_riport_time = 0;
  this->last_xr_rfc7243_riport_time = 0;
  this->last_sender_riport_time = 0;
  this->media_rate = 0.;
  this->goodput = 0;
  this->late_discarded_bytes = 0;
  this->late_discarded_bytes_sum = 0;
  this->early_discarded_bytes = 0;
  this->early_discarded_bytes_sum = 0;

  this->last_packet_count_for_goodput = 0;
  this->last_packet_count_for_sr = 0;
  this->actual_packet_count = 0;
  this->last_payload_bytes_for_goodput = 0;
  this->last_payload_bytes_for_sr = 0;
  this->actual_sent_payload_bytes = 0;
}

guint32
_uint32_diff (guint32 start, guint32 end)
{
  if (start <= end) {
    return end - start;
  }
  return ~((guint32) (start - end));
}


guint16
_uint16_diff (guint16 start, guint16 end)
{
  if (start <= end) {
    return end - start;
  }
  return ~((guint16) (start - end));
}



//------------------ Riport Processing and evaluation -------------------

void
_report_processing_selector (Subflow * this, GstMPRTCPSubflowBlock * block)
{
  guint8 pt;

  gst_rtcp_header_getdown (&block->block_header, NULL, NULL, NULL, &pt, NULL,
      NULL);

  if (pt == (guint8) GST_RTCP_TYPE_RR) {
    _riport_processing_rrblock_processor (this, &block->receiver_riport.blocks);
  } else if (pt == (guint8) GST_RTCP_TYPE_XR) {
    _riport_processing_xrblock_processor (this, &block->xr_rfc7243_riport);
  } else {
    GST_WARNING ("Sending flow control can not process report type %d.", pt);
  }
}

void
_riport_processing_rrblock_processor (Subflow * this, GstRTCPRRBlock * rrb)
{
  GstClockTime LSR, DLSR;
  GstClockTime now;
  guint32 LSR_read, DLSR_read, HSSN_read;
  guint16 HSN_diff;
  guint16 HSSN;
  gfloat lost_rate;
  //RiportProcessorRRProcessor *this_stage = _riport_processing_rrprocessor_;

  now = gst_clock_get_time (this->sysclock);
  //--------------------------
  //validating
  //--------------------------
  gst_rtcp_rrb_getdown (rrb, NULL, NULL, NULL, &HSSN_read, NULL, &LSR_read,
      &DLSR_read);

  HSSN = (guint16) (HSSN_read & 0x0000FFFF);
  this->HSSN_diff = HSN_diff = _uint16_diff (this->HSSN, HSSN);
  this->HSSN = HSSN;
  LSR = (now & 0xFFFF000000000000ULL) | (((guint64) LSR_read) << 16);
  DLSR = (guint64) DLSR_read *GST_MSECOND;

  if (HSN_diff > 32767) {
    GST_WARNING_OBJECT (this, "Receiver riport validation failed "
        "on subflow %d "
        "due to HSN difference inconsistency "
        "(last HSSN: %d, current HSSN: %d)", this->id, this->HSSN, HSSN);
    this->HSSN = HSSN;
    return;
  }

  if (this->first_reicever_riport_arrived && (LSR == 0 || DLSR == 0)) {
    return;
  }
  //--------------------------
  //processing
  //--------------------------
  if (LSR > 0) {
    guint64 diff = now - LSR;
    //g_print("Diff: %lu, DLSR: %lu\n", diff, DLSR);
    if (DLSR < diff) {
      this->RTT = diff - DLSR;
    }
  }

  if (rrb->fraction_lost > 0) {
    ++this->consecutive_lost;
    this->consecutive_non_lost = 0;
  } else {
    this->consecutive_lost = 0;
    ++this->consecutive_non_lost;
  }
  this->lost_rate = lost_rate = ((gfloat) rrb->fraction_lost) / 256.;
  this->first_reicever_riport_arrived = TRUE;
  this->last_receiver_riport_time = now;

//gst_print_rtcp_rrb(rrb);
  //Debug print
  g_print ("Receiver riport is processed, the calculations are the following:\n"
      "lost_rate: %f\n"
      "consecutive_lost: %d\n"
      "consecutive_non_lost: %d\n"
      "RTT (in ms): %lu\n",
      this->lost_rate,
      this->consecutive_lost,
      this->consecutive_non_lost, GST_TIME_AS_MSECONDS (this->RTT));

}


void
_riport_processing_xrblock_processor (Subflow * this, GstRTCPXR_RFC7243 * xrb)
{
  GstClockTime now;
  guint8 interval_metric;
  guint32 discarded_bytes;
  gboolean early_bit;

  now = gst_clock_get_time (this->sysclock);

  gst_rtcp_xr_rfc7243_getdown (xrb, &interval_metric,
      &early_bit, NULL, &discarded_bytes);

  if (interval_metric == RTCP_XR_RFC7243_I_FLAG_CUMULATIVE_DURATION) {
    if (early_bit) {
      this->early_discarded_bytes =
          discarded_bytes - this->early_discarded_bytes_sum;
      this->early_discarded_bytes_sum = discarded_bytes;
    } else {
      this->late_discarded_bytes =
          discarded_bytes - this->late_discarded_bytes_sum;
      this->late_discarded_bytes_sum = discarded_bytes;
    }
  } else if (interval_metric == RTCP_XR_RFC7243_I_FLAG_INTERVAL_DURATION) {
    if (early_bit) {
      this->early_discarded_bytes = discarded_bytes;
      this->early_discarded_bytes_sum += this->early_discarded_bytes;
    } else {
      this->late_discarded_bytes = discarded_bytes;
      this->late_discarded_bytes_sum += this->late_discarded_bytes;
    }
  } else if (interval_metric == RTCP_XR_RFC7243_I_FLAG_SAMPLED_METRIC) {

  }

  if (discarded_bytes > 0) {
    ++this->consecutive_discarded;
    this->consecutive_non_discarded = 0;
  } else {
    this->consecutive_discarded = 0;
    ++this->consecutive_non_discarded;
  }
  this->last_xr_rfc7243_riport_time = now;
}


void
_recalc (SndEventBasedController * this)
{
  GHashTableIter iter;
  gpointer key, val;
  Subflow *subflow;
  MPRTPSPath *path;
  gboolean is_non_congested;
  gboolean is_non_lossy;
  gfloat max_bid = 1000.;
  gfloat allocated_bid = 0.;
  gfloat sending_bid;

  g_hash_table_iter_init (&iter, this->subflows);
  while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & val)) {
    subflow = (Subflow *) val;
    path = subflow->path;
    if (!mprtps_path_is_active (path)) {
      continue;
    }
    _validate_cross_reports_data (subflow);
    subflow->goodput = _get_goodput (subflow);
    is_non_congested = mprtps_path_is_non_congested (path);
    is_non_lossy = mprtps_path_is_non_lossy (path);
    if (is_non_congested && is_non_lossy) {
      //not congested, not lossy
      sending_bid = subflow->goodput / this->goodput_nc_sum *
          (max_bid - allocated_bid) * _get_gamma (subflow);
    } else if (is_non_congested) {
      //not congested but lossy
      sending_bid = MIN (subflow->goodput / this->goodput_mc_sum * max_bid,
          subflow->goodput * (1. - _get_beta (subflow)));
    } else {
      //congested
      sending_bid = MIN (subflow->goodput / this->goodput_c_sum * max_bid,
          subflow->goodput * (1. - _get_alpha (subflow)));
    }
    allocated_bid += sending_bid;
    stream_splitter_setup_sending_bid (this->splitter, subflow->id,
        (guint32) (sending_bid + .5));
  }

}

gfloat
_get_alpha (Subflow * subflow)
{
  gfloat result;
  GST_DEBUG ("get alpha value for subflow %d", subflow->id);
  result = .5;
  return result;
}

gfloat
_get_beta (Subflow * subflow)
{
  gfloat result;
  GST_DEBUG ("get beta value for subflow %d", subflow->id);
  result = .2;
  return result;
}

gfloat
_get_gamma (Subflow * subflow)
{
  gfloat result = 1.;
  result += subflow->for_bid_calc.increasement;
  subflow->for_bid_calc.increasement = 0.;
  result -= subflow->for_bid_calc.decreasement;
  subflow->for_bid_calc.decreasement = 0.;
  return result;
}

void
_fire (SndEventBasedController * this, Subflow * subflow, Event event)
{
  MPRTPSPath *path;
  MPRTPSPathState path_state;
  path = subflow->path;
  path_state = mprtps_path_get_state (path);
  g_print ("FIRE->%d:%d\n", subflow->id, event);
  //passive state
  if (path_state == MPRTPS_PATH_STATE_PASSIVE) {
    switch (event) {
      case EVENT_SETTLED:
        mprtps_path_set_active (path);
        stream_splitter_add_path (this->splitter, subflow->id, path);
        break;
      case EVENT_FI:
      default:
        break;
    }
    goto done;
  }
  //whichever state we are the LATE event means the same
  if (event == EVENT_LATE) {
    mprtps_path_set_passive (path);
    stream_splitter_rem_path (this->splitter, subflow->id);
    goto done;
  }

  if (path_state == MPRTPS_PATH_STATE_NON_CONGESTED) {
    switch (event) {
      case EVENT_CONGESTION:
        subflow->for_bid_calc.decreasement = 0.;
        mprtps_path_set_congested (path);
        this->bids_recalc_requested = TRUE;
        break;
      case EVENT_TRY:
        subflow->for_bid_calc.increasement = 0.;
        this->bids_recalc_requested = TRUE;
        break;
      case EVENT_DISTORTION:
        subflow->for_bid_calc.decreasement = 0.;
        this->bids_recalc_requested = TRUE;
        break;
      case EVENT_LOSSY:
        mprtps_path_set_lossy (path);
        this->bids_commit_requested = TRUE;
        break;
      case EVENT_FI:
      default:
        break;
    }
    goto done;
  }
  //lossy path
  if (path_state == MPRTPS_PATH_STATE_MIDDLY_CONGESTED) {
    switch (event) {
      case EVENT_SETTLED:
        mprtps_path_set_non_lossy (path);
        this->bids_commit_requested = TRUE;
        break;
      case EVENT_CONGESTION:
        mprtps_path_set_non_lossy (path);
        mprtps_path_set_congested (path);
        this->bids_commit_requested = TRUE;
        break;
      case EVENT_FI:
      default:
        break;
    }
    goto done;
  }
  //the path is congested
  switch (event) {
    case EVENT_SETTLED:
      mprtps_path_set_non_congested (path);
      this->bids_commit_requested = TRUE;
      break;
    case EVENT_DISTORTION:
      subflow->for_bid_calc.decreasement = 0.;
      this->bids_recalc_requested = TRUE;
      break;
    case EVENT_FI:
    default:
      break;
  }
done:
  return;
}

Event
_check_riport_timeout (Subflow * this)
{
  Event result = EVENT_FI;
  MPRTPSPath *path;
  GstClockTime now;
  MPRTPSPathState path_state;

  path = this->path;
  path_state = mprtps_path_get_state (path);
  now = gst_clock_get_time (this->sysclock);
  if (path_state == MPRTPS_PATH_STATE_PASSIVE) {
    goto done;
  }
  if (!this->first_reicever_riport_arrived) {
    if (this->joined_time < now - RIPORT_TIMEOUT) {
      result = EVENT_LATE;
    }
    goto done;
  }
  if (this->last_receiver_riport_time < now - RIPORT_TIMEOUT) {
    result = EVENT_LATE;
  }
done:
  return result;
}

Event
_check_state (Subflow * this)
{
  Event result = EVENT_FI;
  GstClockTime sent_passive;
  GstClockTime now;
  MPRTPSPathState path_state;

  path_state = mprtps_path_get_state (this->path);
  now = gst_clock_get_time (this->sysclock);
  switch (path_state) {
    case MPRTPS_PATH_STATE_NON_CONGESTED:
      if (PATH_RTT_MAX_TRESHOLD < this->RTT) {
        result = EVENT_LATE;
      } else if (this->consecutive_lost > 2 && this->consecutive_discarded > 2) {
        result = EVENT_CONGESTION;
      } else if (this->consecutive_lost > 2
          && this->consecutive_non_discarded > 2) {
        result = EVENT_LOSSY;
      } else if (this->consecutive_lost > 1 && this->consecutive_discarded > 0) {
        result = EVENT_DISTORTION;
      } else if (this->consecutive_non_lost > 2) {
        result = EVENT_TRY;
      }
      break;
    case MPRTPS_PATH_STATE_MIDDLY_CONGESTED:
      if (PATH_RTT_MAX_TRESHOLD < this->RTT) {
        result = EVENT_LATE;
      } else if (this->consecutive_non_lost > 2) {
        result = EVENT_SETTLED;
      } else if (this->consecutive_discarded > 1) {
        result = EVENT_CONGESTION;
      }
      break;
    case MPRTPS_PATH_STATE_CONGESTED:
      if (PATH_RTT_MAX_TRESHOLD < this->RTT) {
        result = EVENT_LATE;
      } else if (this->consecutive_non_lost > 2
          && this->consecutive_non_discarded > 2) {
        result = EVENT_SETTLED;
      } else if (this->consecutive_non_discarded > 2) {
        result = EVENT_LOSSY;
      } else if (this->consecutive_discarded > 1) {
        result = EVENT_DISTORTION;
      }
      break;
    case MPRTPS_PATH_STATE_PASSIVE:
      sent_passive = mprtps_path_get_time_sent_to_passive (this->path);
      if (this->RTT < PATH_RTT_MIN_TRESHOLD &&
          sent_passive < now - GST_SECOND * 20) {
        result = EVENT_SETTLED;
      }
      break;
    default:
      break;
  }
  return result;
}

void
_validate_cross_reports_data (Subflow * this)
{

  if (this->last_xr_rfc7243_riport_time <
      this->last_receiver_riport_time - 10 * GST_SECOND) {
    //invalid xr reports.
    this->late_discarded_bytes = 0;
    this->early_discarded_bytes = 0;
    this->consecutive_discarded = 0;
    this->consecutive_non_discarded = 0;
  }
//done:
  return;
}

gfloat
_get_goodput (Subflow * this)
{
  GstClockTimeDiff interval;
  GstClockTime mseconds, now;
  MPRTPSPath *path;
  guint32 payload_bytes_sum = 0;
  guint32 discarded_bytes;
  gfloat result;

  now = gst_clock_get_time (this->sysclock);
  interval = GST_CLOCK_DIFF (this->last_receiver_riport_time, now);
  mseconds = GST_TIME_AS_MSECONDS ((GstClockTime) interval);
  path = this->path;

  this->actual_sent_payload_bytes =
      mprtps_path_get_total_sent_payload_bytes (path);
  payload_bytes_sum =
      _uint32_diff (this->last_payload_bytes_for_goodput,
      this->actual_sent_payload_bytes);
  this->last_payload_bytes_for_goodput = this->actual_sent_payload_bytes;
  discarded_bytes = this->early_discarded_bytes + this->late_discarded_bytes;

  result = ((gfloat) payload_bytes_sum *
      (1. - this->lost_rate) - (gfloat) discarded_bytes) / ((gfloat) mseconds);
  return result;
}

void
_setup_sr_riport (Subflow * this, GstRTCPSR * sr, guint32 ssrc)
{
  guint64 ntptime;
  guint32 rtptime;
  guint32 packet_count, payload_bytes;
  MPRTPSPath *path;

  gst_rtcp_header_change (&sr->header, NULL, NULL, NULL, NULL, NULL, &ssrc);
  ntptime = gst_clock_get_time (this->sysclock);

  rtptime = (guint32) (gst_rtcp_ntp_to_unix (ntptime) >> 32);   //rtptime
  path = this->path;

  this->actual_packet_count = mprtps_path_get_total_sent_packet_num (path);
  packet_count = _uint32_diff (this->last_packet_count_for_sr,
      this->actual_packet_count);

  this->last_packet_count_for_sr = this->actual_packet_count;

  this->actual_sent_payload_bytes =
      mprtps_path_get_total_sent_payload_bytes (path);
  payload_bytes =
      _uint32_diff (this->last_payload_bytes_for_sr,
      this->actual_sent_payload_bytes);

  this->last_payload_bytes_for_sr = this->actual_sent_payload_bytes;

  gst_rtcp_srb_setup (&sr->sender_block, ntptime, rtptime,
      packet_count, payload_bytes >> 3);
}

#undef DEBUG_MODE_ON
#undef MAX_RIPORT_INTERVAL
#undef RIPORT_TIMEOUT
#undef PATH_RTT_MAX_TRESHOLD
#undef PATH_RTT_MIN_TRESHOLD
#undef MAX_RIPORT_INTERVAL
#undef THIS_READLOCK
#undef THIS_READUNLOCK
#undef THIS_WRITELOCK
#undef THIS_WRITEUNLOCK
