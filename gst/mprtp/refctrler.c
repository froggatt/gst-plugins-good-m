/* GStreamer Scheduling tree
 * Copyright (C) 2015 Balázs Kreith (contact: balazs.kreith@gmail.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be ureful,
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
#include "refctrler.h"
#include "streamsplitter.h"
#include "gstmprtcpbuffer.h"
#include "mprtprpath.h"
#include <math.h>

#define THIS_READLOCK(this) g_rw_lock_reader_lock(&this->rwmutex)
#define THIS_READUNLOCK(this) g_rw_lock_reader_unlock(&this->rwmutex)
#define THIS_WRITELOCK(this) g_rw_lock_writer_lock(&this->rwmutex)
#define THIS_WRITEUNLOCK(this) g_rw_lock_writer_unlock(&this->rwmutex)

GST_DEBUG_CATEGORY_STATIC (refctrler_debug_category);
#define GST_CAT_DEFAULT refctrler_debug_category

G_DEFINE_TYPE (RcvEventBasedController, refctrler, G_TYPE_OBJECT);

#define NORMAL_RIPORT_PERIOD_TIME (5*GST_SECOND)

typedef struct _Subflow Subflow;

struct _Subflow
{
  MPRTPRPath *path;
  guint8 id;
  GstClock *sysclock;
  GstClockTime joined_time;
  guint16 lost_packets_num;
  guint32 lost_packets_num_total;
  guint16 discarded_packet_num;
  GstClockTime riport_time;
  GstClockTime riport_interval_time;
  gboolean rr_started;
  guint32 packet_received;
  gfloat media_bw_avg;
  gboolean allow_early;
  gboolean paths_congestion_riport_is_started;
  gboolean paths_changing_riport_started_time;
  guint packet_limit_to_riport;
  gboolean urgent_riport_is_requested;
  GstClockTime last_riport_sent_time;
  GstClockTime LSR;
  guint16 HSN;
  gfloat avg_rtcp_size;
};

//----------------------------------------------------------------------
//-------- Private functions belongs to Scheduler tree object ----------
//----------------------------------------------------------------------

static void refctrler_finalize (GObject * object);
static void refctrler_run (void *data);
static gboolean _do_subflow_riport_now (Subflow * subflow);
static void _setup_rr_riport (Subflow * this, GstRTCPRR * rr);
static guint16 _uint16_diff (guint16 a, guint16 b);
static void refctrler_receive_mprtcp (gpointer subflow, GstBuffer * buf);
static void _refresh (RcvEventBasedController * this, Subflow * subflow);
static void _riport_processing_selector (Subflow * this,
    GstMPRTCPSubflowBlock * block);
static void _riport_processing_srblock_processor (Subflow * subflow,
    GstRTCPSRBlock * srb);
static void _refresh_rtcp_avg_size (Subflow * this, guint packet_size);
static GstBuffer *_gen_rr_riport (Subflow * this, guint32 ssrc);
void _setup_xr_rfc2743_late_discarded_riport (Subflow * this, guint32 ssrc,
    GstMPRTCPSubflowRiport * riport);

static void refctrler_rem_path (gpointer controller_ptr, guint8 subflow_id);
static void refctrler_add_path (gpointer controller_ptr, guint8 subflow_id,
    MPRTPRPath * path);

static void refctrler_riport_can_flow (gpointer subflow);
//subflow functions
static Subflow *make_subflow (guint8 id, MPRTPRPath * path);
static void ruin_subflow (gpointer * subflow);
static void reset_subflow (Subflow * subflow);
static Subflow *subflow_ctor (void);
static void subflow_dtor (Subflow * this);
//----------------------------------------------------------------------
//--------- Private functions implementations to SchTree object --------
//----------------------------------------------------------------------


void
refctrler_class_init (RcvEventBasedControllerClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = refctrler_finalize;

  GST_DEBUG_CATEGORY_INIT (refctrler_debug_category, "refctrler", 0,
      "MpRTP Receiving Event Flow Riporter");

}

void
refctrler_finalize (GObject * object)
{
  RcvEventBasedController *this = REFCTRLER (object);
  g_hash_table_destroy (this->subflows);
  gst_task_stop (this->thread);
  gst_task_join (this->thread);

  g_object_unref (this->sysclock);
}

void
refctrler_init (RcvEventBasedController * this)
{
  this->sysclock = gst_system_clock_obtain ();
  this->subflows = g_hash_table_new_full (NULL, NULL,
      NULL, (GDestroyNotify) ruin_subflow);
  this->ssrc = g_random_int ();
  this->riport_is_flowable = FALSE;

  g_rw_lock_init (&this->rwmutex);
  g_rec_mutex_init (&this->thread_mutex);
  this->thread = gst_task_new (refctrler_run, this, NULL);
  gst_task_set_lock (this->thread, &this->thread_mutex);
  gst_task_start (this->thread);

}

void
refctrler_run (void *data)
{
  GstClockTime now, next_scheduler_time;
  RcvEventBasedController *this;
  GHashTableIter iter;
  gpointer key, val;
  Subflow *subflow;
  GstBuffer *buf;
  GstClockID clock_id;
  this = REFCTRLER (data);
  THIS_WRITELOCK (this);
  now = gst_clock_get_time (this->sysclock);

  g_hash_table_iter_init (&iter, this->subflows);
  while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & val)) {
    subflow = (Subflow *) val;
    _refresh (this, subflow);
    if (_do_subflow_riport_now (subflow) && this->riport_is_flowable) {
      buf = _gen_rr_riport (subflow, this->ssrc);
      this->send_mprtcp_packet_func (this->send_mprtcp_packet_data, buf);
    }
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
_refresh (RcvEventBasedController * this, Subflow * subflow)
{
  MPRTPRPath *path;
  path = subflow->path;
  subflow->discarded_packet_num += mprtpr_path_get_late_discarded_num (path);
  subflow->lost_packets_num += mprtpr_path_get_packet_losts_num (path);
  subflow->packet_received +=
      mprtpr_path_get_packet_received_num_for_riports (path);

}


void
refctrler_add_path (gpointer controller_ptr, guint8 subflow_id,
    MPRTPRPath * path)
{
  RcvEventBasedController *this;
  Subflow *lookup_result;
  this = REFCTRLER (controller_ptr);
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
exit:
  THIS_WRITEUNLOCK (this);
}

void
refctrler_rem_path (gpointer controller_ptr, guint8 subflow_id)
{
  RcvEventBasedController *this;
  Subflow *lookup_result;
  this = REFCTRLER (controller_ptr);
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
exit:
  THIS_WRITEUNLOCK (this);
}


void
refctrler_set_callbacks (void (**riport_can_flow_indicator) (gpointer),
    void (**controller_add_path) (gpointer, guint8, MPRTPRPath *),
    void (**controller_rem_path) (gpointer, guint8))
{
  if (riport_can_flow_indicator) {
    *riport_can_flow_indicator = refctrler_riport_can_flow;
  }
  if (controller_add_path) {
    *controller_add_path = refctrler_add_path;
  }
  if (controller_rem_path) {
    *controller_rem_path = refctrler_rem_path;
  }
}



GstBufferReceiverFunc
refctrler_setup_mprtcp_exchange (RcvEventBasedController * this,
    gpointer data, GstBufferReceiverFunc func)
{
  GstBufferReceiverFunc result;
  THIS_WRITELOCK (this);
  this->send_mprtcp_packet_func = func;
  this->send_mprtcp_packet_data = data;
  result = refctrler_receive_mprtcp;
  THIS_WRITEUNLOCK (this);
  return result;
}

void
refctrler_receive_mprtcp (gpointer ptr, GstBuffer * buf)
{
  GstRTCPHeader *header;
  GstMPRTCPSubflowRiport *riport;
  GstMPRTCPSubflowBlock *block;
  RcvEventBasedController *this = REFCTRLER (ptr);
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
refctrler_riport_can_flow (gpointer ptr)
{
  RcvEventBasedController *this;
  this = REFCTRLER (ptr);
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
make_subflow (guint8 id, MPRTPRPath * path)
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
  this->lost_packets_num = 0;
  this->lost_packets_num_total = 0;
  this->discarded_packet_num = 0;
  this->riport_time = 0;
  this->riport_interval_time = 0;
  this->rr_started = FALSE;
  this->packet_received = 0;
  this->media_bw_avg = 0.;
  this->allow_early = TRUE;
  this->paths_congestion_riport_is_started = FALSE;
  this->paths_changing_riport_started_time = 0;
  this->packet_limit_to_riport = 0;
  this->urgent_riport_is_requested = FALSE;
  this->last_riport_sent_time = 0;
  this->LSR = 0;
  this->HSN = 0;
  this->avg_rtcp_size = 0.;
}


gboolean
_do_subflow_riport_now (Subflow * subflow)
{
  GstClockTime now;
  gboolean result = FALSE;
  GstClockTime t_normal, t_max = 7 * GST_SECOND + 500 * GST_MSECOND;
  GstClockTime t_bw_min;
  gfloat riport_bw;

  gdouble randv = g_random_double_range (0.5, 1.5);
  now = gst_clock_get_time (subflow->sysclock);
  if (!subflow->rr_started) {
    subflow->riport_interval_time = 1 * GST_SECOND;
    subflow->riport_time = now + subflow->riport_interval_time;
    subflow->rr_started = TRUE;
    goto done;
  }

  if (subflow->urgent_riport_is_requested) {
    subflow->urgent_riport_is_requested = FALSE;
    if (!subflow->allow_early) {
      goto done;
    }
    subflow->allow_early = FALSE;
    result = TRUE;
    subflow->riport_interval_time =
        (gdouble) subflow->riport_interval_time / 2. * randv;
    if (subflow->riport_interval_time < 500 * GST_MSECOND) {
      subflow->riport_interval_time = 750. * (gdouble) GST_MSECOND *randv;
    } else if (2 * GST_SECOND < subflow->riport_interval_time) {
      subflow->riport_interval_time = 2. * (gdouble) GST_SECOND *randv;
    }
    subflow->riport_time = now + subflow->riport_interval_time;
    goto done;
  }

  if (now < subflow->riport_time) {
    goto done;
  }


  if (subflow->media_bw_avg > 0.) {
    riport_bw = subflow->media_bw_avg * 0.01;
    t_bw_min =
        (GstClockTime) (subflow->avg_rtcp_size * 2. / riport_bw *
        (gdouble) GST_SECOND);
    t_normal = MAX (NORMAL_RIPORT_PERIOD_TIME, t_bw_min);
  } else {
    t_normal = NORMAL_RIPORT_PERIOD_TIME;
  }

  if (subflow->packet_received < subflow->packet_limit_to_riport) {
    if (subflow->last_riport_sent_time + ((GstClockTime) (t_normal * 3)) < now) {
      result = TRUE;
      subflow->riport_interval_time =
          (GstClockTime) ((gdouble) t_normal * randv);
      subflow->riport_time = now + subflow->riport_time;
      goto done;
    }
    subflow->riport_time = now + (GstClockTime) (t_normal *
        (1.1 - subflow->packet_received / subflow->packet_limit_to_riport));
    subflow->riport_interval_time = subflow->riport_interval_time * 1.5;
    if (t_max < subflow->riport_interval_time) {
      subflow->riport_interval_time = t_max;
    }
    goto done;
  }

  result = TRUE;
  subflow->allow_early = TRUE;
  if (subflow->lost_packets_num > 0 || subflow->discarded_packet_num > 0) {
    if (!subflow->paths_congestion_riport_is_started) {
      subflow->paths_changing_riport_started_time = now;
    }
    subflow->paths_congestion_riport_is_started = TRUE;
    //the change happened in between 10 seconds
    if (now - 10 * GST_SECOND < subflow->paths_changing_riport_started_time) {
      subflow->riport_interval_time =
          (gdouble) subflow->riport_interval_time / 2. * randv;
    } else {
      subflow->riport_interval_time = (gdouble) t_normal *randv;
    }
    if (subflow->riport_interval_time < 500 * GST_MSECOND) {
      subflow->riport_interval_time = (gdouble) (750 * GST_MSECOND) * randv;
    }
  } else {
    if (subflow->paths_congestion_riport_is_started) {
      subflow->paths_congestion_riport_is_started = FALSE;
      subflow->paths_changing_riport_started_time = now;
    }
    if (now - 10 * GST_SECOND < subflow->paths_changing_riport_started_time) {
      subflow->riport_interval_time =
          (gdouble) subflow->riport_interval_time / 2. * randv;
    } else {
      subflow->riport_interval_time =
          (gdouble) subflow->riport_interval_time * 1.5 * randv;
    }
    if (subflow->riport_interval_time < 500 * GST_MSECOND) {
      subflow->riport_interval_time = (gdouble) (750 * GST_MSECOND) * randv;
    } else if (t_max < subflow->riport_interval_time) {
      subflow->riport_interval_time = (gdouble) t_normal *randv;
    }
  }

  subflow->riport_time = now + subflow->riport_interval_time;

done:
  //    g_print("subflow %d this->rr_riport_time = %llu + %llu\n", this->id, now,
  //                      GST_TIME_AS_MSECONDS(this->rr_riport_interval));
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



void
_setup_rr_riport (Subflow * this, GstRTCPRR * rr)
{
  GstClockTime ntptime;
  guint8 fraction_lost;
  guint32 ext_hsn, LSR, DLSR;
  guint16 expected;
  MPRTPRPath *path;
  guint16 HSN;
  guint16 cycle_num;
  gfloat avg_rtp_size;
  guint32 jitter;

  ntptime = gst_clock_get_time (this->sysclock);
  path = this->path;
  HSN = mprtpr_path_get_highest_sequence_number (path);
  cycle_num = mprtpr_path_get_cycle_num (path);
  avg_rtp_size = mprtpr_path_get_avg_rtp_size (path);
  jitter = mprtpr_path_get_jitter (path);

  expected = _uint16_diff (this->HSN, HSN);

  fraction_lost =
      (256.0 * (gfloat) this->lost_packets_num) / ((gfloat) (expected));
  this->lost_packets_num_total += (guint32) this->lost_packets_num;

  ext_hsn = (((guint32) cycle_num) << 16) | ((guint32) HSN);
  //g_print("this->LSR: %016llX -> %016llX\n", this->LSR, (guint32)(this->LSR>>16));
  LSR = (guint32) (this->LSR >> 16);

  if (this->LSR == 0 || ntptime < this->LSR) {
    DLSR = 0;
  } else {
    DLSR = (guint32) GST_TIME_AS_MSECONDS ((GstClockTime)
        GST_CLOCK_DIFF (this->LSR, ntptime));
  }

  gst_rtcp_rr_add_rrb (rr, 0,
      fraction_lost, this->lost_packets_num_total, ext_hsn, jitter, LSR, DLSR);
  //reset
  if (this->last_riport_sent_time > 0) {
    this->media_bw_avg += (avg_rtp_size * (gdouble) this->packet_received /
        (gdouble) (GST_TIME_AS_MSECONDS (ntptime -
                this->last_riport_sent_time) * 1. / 1000.) -
        this->media_bw_avg) * 1. / 16.;

  }
//  g_print("this->media_bw = %f * %f / %f * 1./1000. = %f\n",
//                this->avg_rtp_size, (gdouble)this->packet_received,
//                (gdouble) (GST_TIME_AS_MSECONDS(ntptime - this->last_riport_sent_time)), this->media_bw_avg);
  this->packet_received = 0;
  this->lost_packets_num = 0;
  this->HSN = HSN;
  this->last_riport_sent_time = ntptime;
}



void
_setup_xr_rfc2743_late_discarded_riport (Subflow * this, guint32 ssrc,
    GstMPRTCPSubflowRiport * riport)
{
  GstMPRTCPSubflowBlock *block;
  GstRTCPXR_RFC7243 *xr;
  guint8 flag = RTCP_XR_RFC7243_I_FLAG_INTERVAL_DURATION;
  gboolean early_bit = FALSE;
  guint32 late_discarded_bytes;
  MPRTPRPath *path;

  path = this->path;
  late_discarded_bytes = mprtpr_path_get_late_discarded_bytes_num (path);

  block = gst_mprtcp_riport_add_block_begin (riport, this->id);
  xr = gst_mprtcp_riport_block_add_xr_rfc2743 (block);
  gst_rtcp_header_change (&xr->header, NULL, NULL, NULL, NULL, NULL, &ssrc);

  gst_rtcp_xr_rfc7243_change (xr, &flag, &early_bit,
      NULL, &late_discarded_bytes);

  gst_mprtcp_riport_add_block_end (riport, block);

  //reset
  late_discarded_bytes = 0;
  this->discarded_packet_num = 0;
}





//------------------ Riport Processing and evaluation -------------------

void
_riport_processing_selector (Subflow * this, GstMPRTCPSubflowBlock * block)
{
  guint8 pt;

  gst_rtcp_header_getdown (&block->block_header, NULL, NULL, NULL, &pt, NULL,
      NULL);

  if (pt == (guint8) GST_RTCP_TYPE_SR) {
    _riport_processing_srblock_processor (this,
        &block->sender_riport.sender_block);
  } else {
    GST_WARNING ("Event Based Flow receive controller "
        "can only process MPRTCP SR riports."
        "The received riport payload type is: %d", pt);
  }
}



void
_riport_processing_srblock_processor (Subflow * this, GstRTCPSRBlock * srb)
{
  GST_DEBUG ("RTCP SR riport arrived for subflow %p->%p", this, srb);
  this->LSR = gst_clock_get_time (this->sysclock);
}

GstBuffer *
_gen_rr_riport (Subflow * this, guint32 ssrc)
{
  GstBuffer *result;
  GstMPRTCPSubflowRiport *riport;
  GstMPRTCPSubflowBlock *block;
  GstRTCPRR *rr;
  GstRTCPBuffer rtcp = GST_RTCP_BUFFER_INIT;
  GstRTCPHeader *header;
  guint16 length;

  result = gst_rtcp_buffer_new (1400);
  gst_rtcp_buffer_map (result, GST_MAP_READWRITE, &rtcp);
  header = gst_rtcp_add_begin (&rtcp);
  riport = gst_mprtcp_add_riport (header);

  block = gst_mprtcp_riport_add_block_begin (riport, this->id);
  rr = gst_mprtcp_riport_block_add_rr (block);
  gst_rtcp_header_change (&rr->header, NULL, NULL, NULL, NULL, NULL, &ssrc);

  _setup_rr_riport (this, rr);

  gst_mprtcp_riport_add_block_end (riport, block);

  if (this->discarded_packet_num > 0) {
    _setup_xr_rfc2743_late_discarded_riport (this, ssrc, riport);
  }

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
#undef THIS_READLOCK
#undef THIS_READUNLOCK
#undef THIS_WRITELOCK
#undef THIS_WRITEUNLOCK
