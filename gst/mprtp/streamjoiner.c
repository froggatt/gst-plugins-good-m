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
#include "streamjoiner.h"
#include <math.h>


GST_DEBUG_CATEGORY_STATIC (stream_joiner_debug_category);
#define GST_CAT_DEFAULT stream_joiner_debug_category

#define THIS_READLOCK(this) g_rw_lock_reader_lock(&this->rwmutex)
#define THIS_READUNLOCK(this) g_rw_lock_reader_unlock(&this->rwmutex)
#define THIS_WRITELOCK(this) g_rw_lock_writer_lock(&this->rwmutex)
#define THIS_WRITEUNLOCK(this) g_rw_lock_writer_unlock(&this->rwmutex)

G_DEFINE_TYPE (StreamJoiner, stream_joiner, G_TYPE_OBJECT);

typedef struct _Subflow Subflow;

struct _Subflow
{
  guint8 id;
  MPRTPRPath *path;
  guint32 received_packets;
};

//----------------------------------------------------------------------
//-------- Private functions belongs to Scheduler tree object ----------
//----------------------------------------------------------------------

static void stream_joiner_finalize (GObject * object);
static void stream_joiner_run (void *data);
static Subflow *make_subflow (MPRTPRPath * path);
static GList *_merge_lists (GList * F, GList * L);
gint _cmp_seq (guint16 x, guint16 y);
//----------------------------------------------------------------------
//--------- Private functions implementations to SchTree object --------
//----------------------------------------------------------------------

void
stream_joiner_class_init (StreamJoinerClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = stream_joiner_finalize;

  GST_DEBUG_CATEGORY_INIT (stream_joiner_debug_category, "stream_joiner", 0,
      "MpRTP Manual Sending Controller");
}

void
stream_joiner_finalize (GObject * object)
{
  StreamJoiner *this = STREAM_JOINER (object);
  g_hash_table_destroy (this->subflows);
  gst_task_stop (this->thread);
  gst_task_join (this->thread);

  g_object_unref (this->sysclock);
}

void
stream_joiner_init (StreamJoiner * this)
{
  this->sysclock = gst_system_clock_obtain ();
  this->subflows = g_hash_table_new_full (NULL, NULL, NULL, g_free);
  this->path_skew_counter = 0;
  this->playout_delay = 0.;

  g_rw_lock_init (&this->rwmutex);
  g_rec_mutex_init (&this->thread_mutex);
  this->thread = gst_task_new (stream_joiner_run, this, NULL);
  gst_task_set_lock (this->thread, &this->thread_mutex);
  gst_task_start (this->thread);

}



void
stream_joiner_run (void *data)
{
  GstClockTime now, next_scheduler_time;
  StreamJoiner *this = STREAM_JOINER (data);
  GList *L, *F;
  GstBuffer *buf;
  GHashTableIter iter;
  gpointer key, val;
  Subflow *subflow;
  //guint8 subflow_id;
  MPRTPRPath *path;
  guint64 path_skew;
  gint i;
  guint64 max_path_skew = 0;
  GstClockID clock_id;
  gboolean new_skew_added = FALSE;
  gboolean stat_produce = FALSE;

  THIS_WRITELOCK (this);
  now = gst_clock_get_time (this->sysclock);

  if (this->subflow_num == 0) {
    next_scheduler_time = now + 100 * GST_MSECOND;
    goto done;
  }
  L = this->queued;
  this->queued = NULL;
  stat_produce = GST_SECOND < now - this->last_stat_produce;
  g_hash_table_iter_init (&iter, this->subflows);
  while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & val)) {
    //printf("key %u ---> %u\n", (guint8)*subflow_id, (MPRTPSPath*)*subflow);
    //subflow_id = *((guint8*) key);
    subflow = (Subflow *) val;
    path = subflow->path;

    F = mprtpr_path_get_packets (path);
    F = g_list_reverse (F);
    subflow->received_packets += g_list_length (F);
    L = _merge_lists (F, L);
    path_skew = mprtpr_path_get_packet_skew_median (path);
    if (path_skew > 0) {
      this->path_skews[this->path_skew_index++] = path_skew;
      if (this->path_skew_index == 256) {
        this->path_skew_index = 0;
      }
      if (this->path_skew_counter < 512) {
        ++this->path_skew_counter;
      }
      new_skew_added = TRUE;
    }

    if (stat_produce) {
      g_print ("On subflow %d received packet num is: %d\n",
          subflow->id, subflow->received_packets);
      subflow->received_packets = 0;
      this->last_stat_produce = now;
    }
  }


  while (L != NULL) {
    buf = L->data;
    this->send_mprtp_packet_func (this->send_mprtp_packet_data, buf);
    L = L->next;
  }

  if (new_skew_added) {

    for (i = 0; i < 256 && i < this->path_skew_counter; ++i) {
      if (max_path_skew < this->path_skews[i]) {
        max_path_skew = this->path_skews[i];
      }
    }

    this->playout_delay =
        ((gfloat) max_path_skew + 124. * this->playout_delay) / 125.;
  }
  if (((guint64) this->playout_delay) < GST_MSECOND) {
    this->playout_delay = 2. * (gfloat) GST_MSECOND;

  }
  //g_print("playout: %lu\n", GST_TIME_AS_MSECONDS((guint64)this->playout_delay));
  next_scheduler_time = now + (guint64) this->playout_delay;

done:
  THIS_WRITEUNLOCK (this);

  clock_id = gst_clock_new_single_shot_id (this->sysclock, next_scheduler_time);

  if (gst_clock_id_wait (clock_id, NULL) == GST_CLOCK_UNSCHEDULED) {
    GST_WARNING_OBJECT (this, "The playout clock wait is interrupted");
  }
  gst_clock_id_unref (clock_id);
}


void
stream_joiner_add_path (StreamJoiner * this, guint8 subflow_id,
    MPRTPRPath * path)
{
  Subflow *lookup_result;
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
      make_subflow (path));
  ++this->subflow_num;
exit:
  THIS_WRITEUNLOCK (this);
}

void
stream_joiner_rem_path (StreamJoiner * this, guint8 subflow_id)
{
  Subflow *lookup_result;
  MPRTPRPath *path = NULL;
  GList *packets;
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
  //if the path has something to say...
  path = lookup_result->path;
  packets = mprtpr_path_get_packets (path);
  if (packets != NULL) {
    this->queued = _merge_lists (packets, this->queued);
  }
  if (--this->subflow_num < 0) {
    this->subflow_num = 0;
  }
exit:
  THIS_WRITEUNLOCK (this);
}

void
stream_joiner_set_sending (StreamJoiner * this, gpointer data,
    void (*func) (gpointer, GstBuffer *))
{
  THIS_WRITELOCK (this);
  this->send_mprtp_packet_data = data;
  this->send_mprtp_packet_func = func;
  THIS_WRITEUNLOCK (this);
}

GList *
_merge_lists (GList * F, GList * L)
{
  GList *head = NULL, *tail = NULL, *p = NULL, **s = NULL;
  GstRTPBuffer F_rtp = GST_RTP_BUFFER_INIT, L_rtp = GST_RTP_BUFFER_INIT;
  GstBuffer *F_buf, *L_buf;
  guint16 s1, s2;

  while (F != NULL && L != NULL) {
    F_buf = F->data;
    L_buf = L->data;
    gst_rtp_buffer_map (F_buf, GST_MAP_READ, &F_rtp);
    gst_rtp_buffer_map (L_buf, GST_MAP_READ, &L_rtp);
    s1 = gst_rtp_buffer_get_seq (&F_rtp);
    s2 = gst_rtp_buffer_get_seq (&L_rtp);
    //g_print("S1: %d S2: %d cmp(s1,s2): %d\n", s1,s2, _cmp_seq(s1, s2));
    if (_cmp_seq (s1, s2) < 0) {
      s = &F;
    } else {
      s = &L;
    }
    gst_rtp_buffer_unmap (&F_rtp);
    gst_rtp_buffer_unmap (&L_rtp);
    //s = (((packet_t*)F->data)->absolute_sequence > ((packet_t*)L->data)->absolute_sequence) ? &F : &L;
    if (head == NULL) {
      head = *s;
      p = NULL;
    } else {
      tail->next = *s;
      tail->prev = p;
      p = tail;
    }
    tail = *s;
    *s = (*s)->next;
  }
  if (head != NULL) {
    if (F != NULL) {
      tail->next = F;
      F->prev = tail;
    } else if (L != NULL) {
      tail->next = L;
      L->prev = tail;
    }
  } else {
    head = (F != NULL) ? F : L;
  }

  return head;
}


gint
_cmp_seq (guint16 x, guint16 y)
{
  if (x == y) {
    return 0;
  }
  if (x < y || (0x8000 < x && y < 0x8000)) {
    return -1;
  }
  return 1;

  //return ((gint16) (x - y)) < 0 ? -1 : 1;
}

Subflow *
make_subflow (MPRTPRPath * path)
{
  Subflow *result = g_malloc0 (sizeof (Subflow));
  result->path = path;
  result->id = mprtpr_path_get_id (path);
  result->received_packets = 0;
  return result;
}


#undef THIS_READLOCK
#undef THIS_READUNLOCK
#undef THIS_WRITELOCK
#undef THIS_WRITEUNLOCK
