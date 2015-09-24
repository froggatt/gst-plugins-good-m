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

#define THIS_READLOCK(this) g_rw_lock_reader_lock(&this->rwmutex)
#define THIS_READUNLOCK(this) g_rw_lock_reader_unlock(&this->rwmutex)
#define THIS_WRITELOCK(this) g_rw_lock_writer_lock(&this->rwmutex)
#define THIS_WRITEUNLOCK(this) g_rw_lock_writer_unlock(&this->rwmutex)

#define MAX_RIPORT_INTERVAL (5 * GST_SECOND)
#define RIPORT_TIMEOUT (3 * MAX_RIPORT_INTERVAL)
#define PASSIVE_PATH_RTT_TRESHOLD (200 * GST_MSECOND)
#define ACTIVE_PATH_RTT_TRESHOLD (400 * GST_MSECOND)
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
  GstClockTime last_evaluation_time;
  GstClockTime last_sender_riport_time;
  gfloat goodput;
  gfloat media_bw_avg;
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

};

//----------------------------------------------------------------------
//-------- Private functions belongs to Scheduler tree object ----------
//----------------------------------------------------------------------

static void sefctrler_finalize (GObject * object);
static void sefctrler_run (void *data);
static gboolean _do_subflow_riport_now (Subflow * this);
static guint16 _uint16_diff (guint16 a, guint16 b);
static void sefctrler_receive_mprtcp (gpointer this, GstBuffer * buf);

static void _riport_processing_rrblock_processor (Subflow * subflow,
    GstRTCPRRBlock * rrb);
static void _riport_processing_xrblock_processor (Subflow * this,
    GstRTCPXR_RFC7243 * xrb);
static void _riport_processing_srblock_processor (Subflow * this,
    GstRTCPSRBlock * srb);
static void _riport_processing_selector (Subflow * this,
    GstMPRTCPSubflowBlock * block);
Event _check (Subflow * this);
void _evaluate (SndEventBasedController * this, Subflow * subflow, Event event);
void _recalc (SndEventBasedController * this);
gfloat _get_goodput (Subflow * this);
GstBuffer *_gen_sr_riport (Subflow * this, guint32 ssrc);
void _refresh_rtcp_avg_size (Subflow * this, guint packet_size);
static void _calculate (Subflow * this);
static gfloat _get_alpha (Subflow * this);
static gfloat _get_beta (Subflow * this);
static gfloat _get_gamma (Subflow * this);


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

  this = SEFCTRLER (data);
  THIS_WRITELOCK (this);
  now = gst_clock_get_time (this->sysclock);

  g_hash_table_iter_init (&iter, this->subflows);
  while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & val)) {
    subflow = (Subflow *) val;
    event = _check (subflow);
    path = subflow->path;
    if (event != EVENT_FI) {
      g_print ("subflow %d EVENT: %d\n", subflow->id, event);
      _evaluate (this, subflow, event);
    }
    if (mprtps_path_is_active (path)) {
      continue;
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
        GST_WARNING_OBJECT (this,
            "Evaluating passive value. Shouldn't be here");
        break;
    }
    if (_do_subflow_riport_now (subflow) && this->riport_is_flowable) {
      //riporting
      buf = _gen_sr_riport (subflow, this->ssrc);
      this->send_mprtcp_packet_func (this->send_mprtcp_packet_data, buf);
    }
  }
  this->goodput_nc_sum = goodput_nc_sum;
  this->goodput_c_sum = goodput_c_sum;
  this->goodput_l_sum = goodput_mc_sum;
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
  GstRTCPHeader *header;
  GstMPRTCPSubflowRiport *riport;
  GstMPRTCPSubflowBlock *block;
  SndEventBasedController *this = SEFCTRLER (ptr);
  guint8 pt;
  guint16 subflow_id;
  guint8 info_type;
  Subflow *subflow;
  GstRTCPBuffer rtcp = { NULL, };

  if (G_UNLIKELY (!gst_rtcp_buffer_map (buf, GST_MAP_READ, &rtcp))) {
    GST_WARNING_OBJECT (this, "The RTP packet is not readable");
    return;
  }
  THIS_WRITELOCK (this);
  for (header = gst_rtcp_get_first_header (&rtcp);
      header != NULL; header = gst_rtcp_get_next_header (&rtcp, header)) {
    gst_rtcp_header_getdown (header, NULL, NULL, NULL, &pt, NULL, NULL);
    if (pt != MPRTCP_PACKET_TYPE_IDENTIFIER) {
      continue;
    }

    riport = (GstMPRTCPSubflowRiport *) header;
    for (block = gst_mprtcp_get_first_block (riport);
        block != NULL; block = gst_mprtcp_get_next_block (riport, block)) {
      gst_mprtcp_block_getdown (&block->info, &info_type, NULL, &subflow_id);
      if (info_type != MPRTCP_BLOCK_TYPE_RIPORT) {
        continue;
      }
      subflow =
          (Subflow *) g_hash_table_lookup (this->subflows,
          GINT_TO_POINTER (subflow_id));
      if (subflow == NULL) {
        GST_WARNING_OBJECT (this,
            "MPRTCP riport can not be binded any "
            "subflow with the given id: %d", subflow_id);
        continue;
      }
      _riport_processing_selector (subflow, block);
    }
  }
  gst_rtcp_buffer_unmap (&rtcp);
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
  this->last_evaluation_time = 0;
  this->media_bw_avg = 0.;
  this->goodput = 0;
  this->late_discarded_bytes = 0;
  this->late_discarded_bytes_sum = 0;
  this->early_discarded_bytes = 0;
  this->early_discarded_bytes_sum = 0;
}


gboolean
_do_subflow_riport_now (Subflow * this)
{
  gboolean result = FALSE;
  GstClockTime t_normal;
  GstClockTime t_bw_min;
  //MPRTPSPath *path;
  GstClockTime now;
  gdouble randv;
  now = gst_clock_get_time (this->sysclock);
  randv = g_random_double_range (0.5, 1.5);
  //path = this->path;
  if (!this->riport_check_started) {
    this->riport_interval_time = 1 * GST_SECOND;
    this->next_riport_time = now + this->riport_interval_time;
    this->riport_check_started = TRUE;
    goto done;
  }

  if (now < this->next_riport_time) {
    goto done;
  }

  if (this->media_bw_avg > 0.) {
    this->sr_riport_bw = this->media_bw_avg * 0.025;
    t_bw_min =
        (GstClockTime) (this->avg_rtcp_size * 2. / this->sr_riport_bw *
        (gdouble) GST_SECOND);
    t_normal = MIN (MAX_RIPORT_INTERVAL, t_bw_min);
    if (t_normal < GST_SECOND) {
      t_normal = GST_SECOND;
    }
  } else {
    t_normal = MAX_RIPORT_INTERVAL;
  }

  result = TRUE;
  this->riport_interval_time = (gdouble) t_normal *randv;

  this->next_riport_time = now + this->riport_interval_time;
done:
  return result;
}

guint16
_uint16_diff (guint16 a, guint16 b)
{
  if (a <= b) {
    return b - a;
  }
  return ~((guint16) (a - b));
}



//------------------ Riport Processing and evaluation -------------------

void
_riport_processing_selector (Subflow * this, GstMPRTCPSubflowBlock * block)
{
  guint8 pt;

  gst_rtcp_header_getdown (&block->block_header, NULL, NULL, NULL, &pt, NULL,
      NULL);

  if (pt == (guint8) GST_RTCP_TYPE_RR) {
    _riport_processing_rrblock_processor (this, &block->receiver_riport.blocks);
  } else if (pt == (guint8) GST_RTCP_TYPE_SR) {
    _riport_processing_srblock_processor (this,
        &block->sender_riport.sender_block);
  } else if (pt == (guint8) GST_RTCP_TYPE_XR) {
    _riport_processing_xrblock_processor (this, &block->xr_rfc7243_riport);
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

  //Debug print
  g_print ("Receiver riport is processed, the calculations are the following:\n"
      "lost_rate: %f\n"
      "consecutive_lost: %d\n"
      "consecutive_non_lost: %d\n"
      "RTT (in ms): %lu",
      this->lost_rate,
      this->consecutive_lost, this->consecutive_non_lost, this->RTT);
}

void
_riport_processing_srblock_processor (Subflow * this, GstRTCPSRBlock * srb)
{
  GST_DEBUG ("RTCP SR riport arrived for subflow %p->%p", this, srb);
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
    is_non_congested = mprtps_path_is_non_congested (path);
    is_non_lossy = mprtps_path_is_non_lossy (path);
    if (is_non_congested && is_non_lossy) {
      //not congested, not lossy
      sending_bid = subflow->goodput / this->goodput_nc_sum *
          (max_bid - allocated_bid) * _get_gamma (subflow);
    } else if (is_non_congested) {
      //not congested but lossy
      sending_bid = MIN (subflow->goodput / this->goodput_l_sum * max_bid,
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
_evaluate (SndEventBasedController * this, Subflow * subflow, Event event)
{
//  gboolean is_non_congested;
//  gboolean is_non_lossy;
//  gboolean is_active;
  MPRTPSPath *path;
  MPRTPSPathState path_state;
  path = subflow->path;
  path_state = mprtps_path_get_state (path);

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
_check (Subflow * this)
{
  Event result = EVENT_FI;
  MPRTPSPath *path;
  GstClockTime sent_passive;
  GstClockTime now;
  MPRTPSPathState path_state;

  now = gst_clock_get_time (this->sysclock);
  path = this->path;
  path_state = mprtps_path_get_state (path);
  if (path_state == MPRTPS_PATH_STATE_PASSIVE) {
    if (this->last_receiver_riport_time < this->last_evaluation_time) {
      goto done;
    }
    _calculate (this);
    sent_passive = mprtps_path_get_time_sent_to_passive (path);
    if (this->RTT < PASSIVE_PATH_RTT_TRESHOLD &&
        sent_passive < now - GST_SECOND * 20) {
      result = EVENT_SETTLED;
    }
    goto done;
  }
  if (!this->first_reicever_riport_arrived) {
    if (this->joined_time < now - RIPORT_TIMEOUT) {
      result = EVENT_LATE;
    }
    goto done;
  }
  if (this->last_receiver_riport_time < this->last_evaluation_time) {
    if (this->last_receiver_riport_time < now - RIPORT_TIMEOUT) {
      result = EVENT_LATE;
    }
    goto done;
  }
  _calculate (this);
  if (path_state == MPRTPS_PATH_STATE_NON_CONGESTED) {
    if (this->consecutive_lost > 2 && this->consecutive_discarded > 2) {
      result = EVENT_CONGESTION;
    } else if (this->consecutive_lost > 2
        && this->consecutive_non_discarded > 2) {
      result = EVENT_LOSSY;
    } else if (this->consecutive_lost > 1 && this->consecutive_discarded > 0) {
      result = EVENT_DISTORTION;
    } else if (this->consecutive_non_lost > 2) {
      result = EVENT_TRY;
    }
    goto done;
  }
  //so the path is lossy
  if (path_state == MPRTPS_PATH_STATE_MIDDLY_CONGESTED) {
    if (this->consecutive_non_lost > 2) {
      result = EVENT_SETTLED;
    } else if (this->consecutive_discarded > 1) {
      result = EVENT_CONGESTION;
    }
    goto done;
  }
  //the path is congested
  if (this->consecutive_non_lost > 2 && this->consecutive_non_discarded > 2) {
    result = EVENT_SETTLED;
  } else if (this->consecutive_non_discarded > 2) {
    result = EVENT_LOSSY;
  } else if (this->consecutive_discarded > 1) {
    result = EVENT_DISTORTION;
  }
done:
  this->last_evaluation_time = now;
  return result;
}

void
_calculate (Subflow * this)
{
  GstClockTimeDiff diff;
  diff = (this->last_receiver_riport_time < this->last_xr_rfc7243_riport_time) ?
      GST_CLOCK_DIFF (this->last_receiver_riport_time,
      this->last_xr_rfc7243_riport_time) :
      GST_CLOCK_DIFF (this->last_xr_rfc7243_riport_time,
      this->last_receiver_riport_time);

  if (500 * GST_MSECOND < (GstClockTime) diff) {
    this->late_discarded_bytes = 0;
    this->early_discarded_bytes = 0;
    this->consecutive_discarded = 0;
    ++this->consecutive_non_discarded;
  }
  this->goodput = _get_goodput (this);

//done:
  return;
}

gfloat
_get_goodput (Subflow * this)
{
  GstClockTimeDiff interval;
  GstClockTime mseconds, now;
  MPRTPSPath *path;
  guint32 payload_bytes_sum;
  guint32 discarded_bytes;
  gfloat result;

  now = gst_clock_get_time (this->sysclock);
  interval = GST_CLOCK_DIFF (this->last_receiver_riport_time, now);
  mseconds = GST_TIME_AS_MSECONDS ((GstClockTime) interval);
  path = this->path;

  payload_bytes_sum = mprtps_path_get_sent_payload_bytes (path);
  discarded_bytes = this->early_discarded_bytes + this->late_discarded_bytes;

  result = ((gfloat) payload_bytes_sum *
      (1. - this->lost_rate) - (gfloat) discarded_bytes) / ((gfloat) mseconds);
  return result;
}


GstBuffer *
_gen_sr_riport (Subflow * this, guint32 ssrc)
{
  GstBuffer *result;
  GstMPRTCPSubflowRiport *riport;
  GstMPRTCPSubflowBlock *block;
  GstRTCPSR *sr;
  guint64 ntptime;
  guint32 rtptime;
  guint32 packet_count, octet_count;
  MPRTPSPath *path;
  GstRTCPBuffer rtcp = GST_RTCP_BUFFER_INIT;
  GstRTCPHeader *header;
  guint16 length;

  result = gst_rtcp_buffer_new (1400);
  gst_rtcp_buffer_map (result, GST_MAP_READWRITE, &rtcp);
  header = gst_rtcp_add_begin (&rtcp);
  riport = gst_mprtcp_add_riport (header);

  ntptime = gst_clock_get_time (this->sysclock);

  rtptime = (guint32) (gst_rtcp_ntp_to_unix (ntptime) >> 32),   //rtptime
      path = this->path;

  block = gst_mprtcp_riport_add_block_begin (riport, this->id);
  sr = gst_mprtcp_riport_block_add_sr (block);
  gst_rtcp_header_change (&sr->header, NULL, NULL, NULL, NULL, NULL, &ssrc);

  packet_count = mprtps_path_get_sent_packet_num (path);
  octet_count = mprtps_path_get_total_sent_payload_bytes (path) >> 3;
  gst_rtcp_srb_setup (&sr->sender_block, ntptime, rtptime,
      packet_count, octet_count);

  gst_mprtcp_riport_add_block_end (riport, block);
  if (this->last_sender_riport_time > 0) {
    this->media_bw_avg += (((gdouble) (octet_count << 3) /
            ((gdouble) GST_TIME_AS_MSECONDS (ntptime -
                    this->last_sender_riport_time) * 1. / 1000.)) -
        this->media_bw_avg) * 1. / 16.;
  }

  this->last_sender_riport_time = ntptime;

  gst_rtcp_add_end (&rtcp, header);
  gst_rtcp_header_getdown (header, NULL, NULL, NULL, NULL, &length, NULL);
  _refresh_rtcp_avg_size (this, length << 2);
  gst_rtcp_buffer_unmap (&rtcp);
  return result;
}

void
_refresh_rtcp_avg_size (Subflow * this, guint packet_size)
{
  this->avg_rtcp_size +=
      ((gdouble) packet_size - this->avg_rtcp_size) * 1. / 16.;
}

#undef MAX_RIPORT_INTERVAL
#undef RIPORT_TIMEOUT
#undef PASSIVE_PATH_RTT_TRESHOLD
#undef ACTIVE_PATH_RTT_TRESHOLD
#undef MAX_RIPORT_INTERVAL
#undef THIS_READLOCK
#undef THIS_READUNLOCK
#undef THIS_WRITELOCK
#undef THIS_WRITEUNLOCK
