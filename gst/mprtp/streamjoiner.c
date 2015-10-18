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
#include <stdlib.h>


//Copied from gst-plugins-base/gst-libs//gst/video/video-format.c
#ifndef restrict
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
/* restrict should be available */
#elif defined(__GNUC__) && __GNUC__ >= 4
#define restrict __restrict__
#elif defined(_MSC_VER) &&  _MSC_VER >= 1500
#define restrict __restrict
#else
#define restrict                /* no op */
#endif
#endif


GST_DEBUG_CATEGORY_STATIC (stream_joiner_debug_category);
#define GST_CAT_DEFAULT stream_joiner_debug_category

#define THIS_READLOCK(this) g_rw_lock_reader_lock(&this->rwmutex)
#define THIS_READUNLOCK(this) g_rw_lock_reader_unlock(&this->rwmutex)
#define THIS_WRITELOCK(this) g_rw_lock_writer_lock(&this->rwmutex)
#define THIS_WRITEUNLOCK(this) g_rw_lock_writer_unlock(&this->rwmutex)


// Returns the biggest element in the heap
//Returns the smallest sequence element in the heap
#define heap_front(h) (*(h)->data)

// Frees the allocated memory
#define heap_term(h) (g_free((h)->data))


//#define CMP(a, b) ((a)->seq_num <= (b)->seq_num)
#define HEAP_CMP(a, b) ((_cmp_seq((a)->seq_num, (b)->seq_num) < 0))

static const unsigned int base_size = 4;


G_DEFINE_TYPE (StreamJoiner, stream_joiner, G_TYPE_OBJECT);

typedef struct _Subflow Subflow;

struct _Subflow
{
  guint8 id;
  MpRTPRPath *path;
  guint32 received_packets;
};

//Heap functions
typedef struct _HeapItem
{
  GstBuffer *buffer;
  guint16 seq_num;
} HeapItem;

struct _Heap
{
  unsigned int size;            // Size of the allocated memory (in number of items)
  unsigned int count;           // Count of the elements in the heap
  HeapItem **data;              // Array with the elements
};

//----------------------------------------------------------------------
//-------- Private functions belongs to Scheduler tree object ----------
//----------------------------------------------------------------------

static void stream_joiner_finalize (GObject * object);
static void stream_joiner_run (void *data);
static Subflow *_make_subflow (MpRTPRPath * path);
void _ruin_subflow (gpointer data);
static gint _cmp_seq (guint16 x, guint16 y);
static void _heap_init (struct _Heap *restrict h);
static void _heap_push (struct _Heap *restrict h, HeapItem * value);
static void _heap_pop (struct _Heap *restrict h);
HeapItem *_make_heap_item (StreamJoiner * this, GstBuffer * buffer,
    guint16 seq_num);
void _trash_heap_item (StreamJoiner * this, HeapItem * heap_item);

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
  g_free (this->packets_heap);
  g_object_unref (this->sysclock);
}

void
stream_joiner_init (StreamJoiner * this)
{
  this->sysclock = gst_system_clock_obtain ();
  this->subflows = g_hash_table_new_full (NULL, NULL, NULL, _ruin_subflow);
  this->playout_delay = 10 * GST_MSECOND;
  this->packets_heap = g_malloc0 (sizeof (Heap));
  this->heap_items_pool = g_queue_new ();

  _heap_init (this->packets_heap);
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
  GstBuffer *buf;
  GHashTableIter iter;
  gpointer key, val;
  Subflow *subflow;
  MpRTPRPath *path;
  guint16 seq_num;
  GList *it;
  HeapItem *heap_item;
  GstClockID clock_id;

  THIS_WRITELOCK (this);
  now = gst_clock_get_time (this->sysclock);

  if (this->subflow_num == 0) {
    next_scheduler_time = now + 100 * GST_MSECOND;
    goto done;
  }
  if (this->queued) {
    for (it = this->queued; it; it = it->next)
      _heap_push (this->packets_heap, (HeapItem *) it->data);
  }

  g_hash_table_iter_init (&iter, this->subflows);
  while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & val)) {
    //printf("key %u ---> %u\n", (guint8)*subflow_id, (MPRTPSPath*)*subflow);
    //subflow_id = *((guint8*) key);
    subflow = (Subflow *) val;
    path = subflow->path;
    while (mprtpr_path_has_buffer_to_playout (path)) {
      buf = mprtpr_path_pop_buffer_to_playout (path, &seq_num);
      heap_item = _make_heap_item (this, buf, seq_num);
      _heap_push (this->packets_heap, heap_item);
    }
  }

  while (this->packets_heap->count) {
    heap_item = heap_front (this->packets_heap);
    _heap_pop (this->packets_heap);
    //g_print ("%hu->", heap_item->seq_num);
    this->send_mprtp_packet_func (this->send_mprtp_packet_data,
        heap_item->buffer);
    _trash_heap_item (this, heap_item);
  }
  //g_print("playout: %lu\n", GST_TIME_AS_MSECONDS((guint64)this->playout_delay));
  next_scheduler_time = now + this->playout_delay;

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
    MpRTPRPath * path)
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
      _make_subflow (path));
  ++this->subflow_num;
exit:
  THIS_WRITEUNLOCK (this);
}

void
stream_joiner_rem_path (StreamJoiner * this, guint8 subflow_id)
{
  Subflow *lookup_result;
  MpRTPRPath *path = NULL;
  GstBuffer *buf;
  guint16 abs_seq;
  HeapItem *heap_item;

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
  while (mprtpr_path_has_buffer_to_playout (path)) {
    buf = mprtpr_path_pop_buffer_to_playout (path, &abs_seq);
    heap_item = _make_heap_item (this, buf, abs_seq);
    this->queued = g_list_prepend (this->queued, heap_item);
  }

  if (--this->subflow_num < 0) {
    this->subflow_num = 0;
  }
exit:
  THIS_WRITEUNLOCK (this);
}

void
stream_joiner_set_playout_delay (StreamJoiner * this,
    GstClockTime playout_delay)
{
  THIS_WRITELOCK (this);
  this->playout_delay = playout_delay;
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
_make_subflow (MpRTPRPath * path)
{
  Subflow *result = g_malloc0 (sizeof (Subflow));
  result->path = path;
  result->id = mprtpr_path_get_id (path);
  result->received_packets = 0;
  return result;
}

void
_ruin_subflow (gpointer data)
{
  Subflow *this;
  this = (Subflow *) data;
  GST_DEBUG_OBJECT (this, "Subflow %d destroyed", this->id);
}


// Prepares the heap for use
void
_heap_init (struct _Heap *restrict h)
{
  *h = (struct _Heap) {
    .size = base_size,.count = 0,.data =
        g_malloc0 (sizeof (HeapItem *) * base_size)
  };
  //if (!h->data) _exit(1); // Exit if the memory allocation fails
}

// Inserts element to the heap
void
_heap_push (struct _Heap *restrict h, HeapItem * value)
{
  unsigned int index, parent;

  // Resize the heap if it is too small to hold all the data
  if (h->count == h->size) {
    h->size <<= 1;
    h->data = realloc (h->data, sizeof (HeapItem *) * h->size);
    //if (!h->data) _exit(1); // Exit if the memory allocation fails
  }
  // Find out where to put the element and put it
  for (index = h->count++; index; index = parent) {
    parent = (index - 1) >> 1;
    if HEAP_CMP
      (h->data[parent], value) break;
    h->data[index] = h->data[parent];
  }
  h->data[index] = value;
}

// Removes the biggest element from the heap
void
_heap_pop (struct _Heap *restrict h)
{
  unsigned int index, swap, other;

  // Remove the biggest element
  HeapItem *temp = h->data[--h->count];

  // Resize the heap if it's consuming too much memory
//        if ((h->count <= (h->size >> 2)) && (h->size > base_size))
//        {
//                h->size >>= 1;
//                h->data = realloc(h->data, sizeof(HeapItem*) * h->size);
//                if (!h->data) _exit(1); // Exit if the memory allocation fails
//        }

  // Reorder the elements
  for (index = 0; 1; index = swap) {
    // Find the child to swap with
    swap = (index << 1) + 1;
    if (swap >= h->count)
      break;                    // If there are no children, the heap is reordered
    other = swap + 1;
    if ((other < h->count) && HEAP_CMP (h->data[other], h->data[swap]))
      swap = other;
    if HEAP_CMP
      (temp, h->data[swap]) break;      // If the bigger child is less than or equal to its parent, the heap is reordered

    h->data[index] = h->data[swap];
  }
  h->data[index] = temp;
}


HeapItem *
_make_heap_item (StreamJoiner * this, GstBuffer * buffer, guint16 seq_num)
{
  HeapItem *result;
  if (g_queue_is_empty (this->heap_items_pool)) {
    result = g_malloc0 (sizeof (HeapItem));
  } else {
    result = (HeapItem *) g_queue_pop_head (this->heap_items_pool);
  }
  result->buffer = buffer;
  result->seq_num = seq_num;
  return result;
}

void
_trash_heap_item (StreamJoiner * this, HeapItem * heap_item)
{
  if (g_queue_get_length (this->heap_items_pool) < 256) {
    g_queue_push_head (this->heap_items_pool, heap_item);
  } else {
    g_free (heap_item);
  }
}


#undef HEAP_CMP
#undef THIS_READLOCK
#undef THIS_READUNLOCK
#undef THIS_WRITELOCK
#undef THIS_WRITEUNLOCK
