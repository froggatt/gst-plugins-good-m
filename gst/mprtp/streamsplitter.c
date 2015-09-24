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
#include "streamsplitter.h"
#include "mprtpspath.h"
#include <math.h>


GST_DEBUG_CATEGORY_STATIC (stream_splitter_debug_category);
#define GST_CAT_DEFAULT stream_splitter_debug_category

#define SCHTREE_MAX_VALUE 128
#define THIS_READLOCK(this) g_rw_lock_reader_lock(&this->rwmutex)
#define THIS_READUNLOCK(this) g_rw_lock_reader_unlock(&this->rwmutex)
#define THIS_WRITELOCK(this) g_rw_lock_writer_lock(&this->rwmutex)
#define THIS_WRITEUNLOCK(this) g_rw_lock_writer_unlock(&this->rwmutex)

/* class initialization */
G_DEFINE_TYPE (StreamSplitter, stream_splitter, G_TYPE_OBJECT);

typedef struct _Subflow Subflow;

struct _SchNode
{
  SchNode *left;
  SchNode *right;
  SchNode *next;
  MPRTPSPath *path;
};

struct _Subflow
{
  MPRTPSPath *path;
  guint32 actual_bid;
  guint32 new_bid;
};

//----------------------------------------------------------------------
//-------- Private functions belongs to Scheduler tree object ----------
//----------------------------------------------------------------------
//Functions realted to stream_splitter
static void stream_splitter_finalize (GObject * object);
static void _full_tree_commit (StreamSplitter * this, guint32 bid_sum);
static void _keyframes_tree_commit (StreamSplitter * this, guint32 bid_sum,
    MPRTPSPathState filter);

//Functions related to tree
static SchNode *_schnode_ctor (void);
static void _schtree_insert (SchNode ** node,
    MPRTPSPath * path, gint * change, gint level_value);
static void _schnode_rdtor (SchNode * node);
static void _schnode_overlap_trees (SchNode * old_node, SchNode * new_node);
//static void _print_tree (SchNode * root, gint top, gint level);
//static MPRTPSPath* schtree_get_actual (SchNode * root);
static MPRTPSPath *schtree_get_next (SchNode * root);
static void stream_splitter_run (void *data);

static Subflow *make_subflow (MPRTPSPath * path);


//----------------------------------------------------------------------
//---- Private function implementations to Stream Dealer object --------
//----------------------------------------------------------------------

void
stream_splitter_class_init (StreamSplitterClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = stream_splitter_finalize;

  GST_DEBUG_CATEGORY_INIT (stream_splitter_debug_category, "stream_splitter", 0,
      "Stream Dealer");

}

void
stream_splitter_finalize (GObject * object)
{
  StreamSplitter *this = STREAM_SPLITTER (object);
  g_hash_table_destroy (this->subflows);
  gst_task_stop (this->thread);
  gst_task_join (this->thread);
  g_object_unref (this->sysclock);
}

void
stream_splitter_init (StreamSplitter * this)
{
  this->new_path_added = FALSE;
  this->changes_are_committed = FALSE;
  this->path_is_removed = FALSE;
  g_rw_lock_init (&this->rwmutex);
  g_rec_mutex_init (&this->thread_mutex);
  this->thread = gst_task_new (stream_splitter_run, this, NULL);
  gst_task_set_lock (this->thread, &this->thread_mutex);
  gst_task_start (this->thread);

  this->sysclock = gst_system_clock_obtain ();
  this->ext_header_id = MPRTP_DEFAULT_EXTENSION_HEADER_ID;
  this->subflows = g_hash_table_new_full (NULL, NULL, NULL, g_free);
  this->charge_value = 1;
}

void
stream_splitter_add_path (StreamSplitter * this, guint8 subflow_id,
    MPRTPSPath * path)
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
  lookup_result = make_subflow (path);
  g_hash_table_insert (this->subflows, GINT_TO_POINTER (subflow_id),
      lookup_result);
  this->new_path_added = TRUE;
exit:
  THIS_WRITEUNLOCK (this);
}

void
stream_splitter_rem_path (StreamSplitter * this, guint8 subflow_id)
{
  Subflow *lookup_result;
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
  this->path_is_removed = TRUE;
exit:
  THIS_WRITEUNLOCK (this);
}

void
stream_splitter_set_rtp_ext_header_id (StreamSplitter * this,
    guint8 ext_header_id)
{
  THIS_WRITELOCK (this);
  this->ext_header_id = ext_header_id;
  THIS_WRITEUNLOCK (this);
}

void
stream_splitter_setup_sending_bid (StreamSplitter * this, guint8 subflow_id,
    guint32 bid)
{
  Subflow *subflow;
  THIS_WRITELOCK (this);
  subflow =
      (Subflow *) g_hash_table_lookup (this->subflows,
      GINT_TO_POINTER (subflow_id));
  if (subflow == NULL) {
    GST_WARNING_OBJECT (this,
        "The requested setup bid operation can not be done "
        "due to not existed subflow id (%d)", subflow_id);
    goto exit;
  }
  subflow->new_bid = bid;
exit:
  THIS_WRITEUNLOCK (this);
}

void
stream_splitter_commit_changes (StreamSplitter * this)
{
  THIS_WRITELOCK (this);
  this->changes_are_committed = TRUE;
  THIS_WRITEUNLOCK (this);
}

//Here the buffer must be writeable and must be rtp
void
stream_splitter_process_rtp_packet (StreamSplitter * this, GstRTPBuffer * rtp)
{
  MPRTPSPath *path;
  THIS_WRITELOCK (this);
  if (this->keyframes_tree == NULL || this->full_tree == NULL) {
    //vagy várni egy kis ideig....
    GST_WARNING_OBJECT (this, "no appropiate tree exists for distributing");
    goto done;
  }

  if (!GST_BUFFER_FLAG_IS_SET (rtp->buffer, GST_BUFFER_FLAG_DELTA_UNIT)) {
    //its a keyframe
    path = schtree_get_next (this->keyframes_tree);
  } else {
    path = schtree_get_next (this->full_tree);
  }

  mprtps_path_process_rtp_packet (path, this->ext_header_id, rtp);
done:
  THIS_WRITEUNLOCK (this);
}

void
stream_splitter_run (void *data)
{
  StreamSplitter *this;
  GstClockTime now;
  GstClockID clock_id;
  GstClockTime next_scheduler_time;
  MPRTPSPathState path_state;
  gdouble rand;
  GHashTableIter iter;
  gpointer key, val;
  Subflow *subflow;
  MPRTPSPath *path;
  guint32 bid_total_sum = 0, bid_nc_sum = 0, bid_l_sum = 0;

  this = STREAM_SPLITTER (data);
  now = gst_clock_get_time (this->sysclock);

  THIS_WRITELOCK (this);


  if (!this->new_path_added &&
      !this->path_is_removed && !this->changes_are_committed) {
    //debug printing
    g_hash_table_iter_init (&iter, this->subflows);
    while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & val)) {
      //subflow_id = *((guint8*) key);
      subflow = (Subflow *) val;
      path = subflow->path;
      g_print ("%p,subflow %d,%d\n", this, mprtps_path_get_id (path),
          mprtps_path_get_sent_packet_num (path));
    }
    goto done;
  }

  g_hash_table_iter_init (&iter, this->subflows);
  while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & val)) {
    //printf("key %u ---> %u\n", (guint8)*subflow_id, (MPRTPSPath*)*subflow);
    //subflow_id = *((guint8*) key);
    subflow = (Subflow *) val;
    path = subflow->path;
    if (mprtps_path_is_new (path)) {
      mprtps_path_set_not_new (path);
      mprtps_path_set_non_congested (path);
      mprtps_path_set_non_lossy (path);
      subflow->actual_bid = this->charge_value;
    }
    if (subflow->new_bid > 0) {
      subflow->actual_bid = subflow->new_bid;
      subflow->new_bid = 0;
    }
    if (subflow->actual_bid == 0) {
      GST_WARNING_OBJECT (this, "The actual bid can not be 0, so it set to 1.");
      subflow->actual_bid = 1;
    }
    bid_total_sum += subflow->actual_bid;
    path_state = mprtps_path_get_state (path);
    if (path_state == MPRTPS_PATH_STATE_NON_CONGESTED) {
      bid_nc_sum += subflow->actual_bid;
    } else if (mprtps_path_is_non_congested (path)) {
      bid_l_sum += subflow->actual_bid;
    }
    //debug printing
    g_print ("%p,subflow %d,%d\n", this, mprtps_path_get_id (path),
        mprtps_path_get_sent_packet_num (path));
  }

  _full_tree_commit (this, bid_total_sum);
  if (bid_nc_sum > 0) {
    _keyframes_tree_commit (this, bid_nc_sum, MPRTPS_PATH_STATE_NON_CONGESTED);
  } else if (bid_l_sum > 0) {
    _keyframes_tree_commit (this, bid_nc_sum,
        MPRTPS_PATH_STATE_MIDDLY_CONGESTED);
  } else {
    this->keyframes_tree = this->full_tree;
  }
//  g_print("full_tree:\n"); _print_tree(this->full_tree, SCHTREE_MAX_VALUE, 0);
//  g_print("keyframes_tree:\n"); _print_tree(this->keyframes_tree, SCHTREE_MAX_VALUE, 0);

  this->new_path_added = FALSE;
  this->path_is_removed = FALSE;
  this->changes_are_committed = FALSE;

done:
  rand = g_random_double ();
  next_scheduler_time = now + GST_SECOND * (0.5 + rand);
  GST_DEBUG_OBJECT (this, "Next scheduling interval time is %lu",
      next_scheduler_time);
  clock_id = gst_clock_new_single_shot_id (this->sysclock, next_scheduler_time);

  THIS_WRITEUNLOCK (this);

  if (gst_clock_id_wait (clock_id, NULL) == GST_CLOCK_UNSCHEDULED) {
    GST_WARNING_OBJECT (this, "The scheduler clock wait is interrupted");
  }
  gst_clock_id_unref (clock_id);
}

//tree functions
static void
_schtree_loadup (SchNode * node, MPRTPSPath * path)
{

  if (node == NULL || node->path != NULL) {
    return;
  }
  if (node->left == NULL) {
    node->left = _schnode_ctor ();
    node->left->path = path;
  } else {
    _schtree_loadup (node->left, path);
  }

  if (node->right == NULL) {
    node->right = _schnode_ctor ();
    node->right->path = path;
  } else {
    _schtree_loadup (node->right, path);
  }
}



void
_full_tree_commit (StreamSplitter * this, guint32 bid_sum)
{
  gdouble actual_value;
  gint insert_value;
  SchNode *new_root = NULL;
  MPRTPSPath *path, *path_with_largest_bid = NULL;
  guint32 largest_bid = 0;
  Subflow *subflow;
  gpointer val, key;
  GHashTableIter iter;
  //guint8 subflow_id;

  g_hash_table_iter_init (&iter, this->subflows);
  while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & val)) {
    //printf("key %u ---> %u\n", (guint8)*subflow_id, (MPRTPSPath*)*subflow);
    //subflow_id = *((guint8*) key);
    subflow = (Subflow *) val;
    path = subflow->path;
    if (largest_bid < subflow->actual_bid) {
      largest_bid = subflow->actual_bid;
      path_with_largest_bid = path;
    }

    actual_value =
        (gdouble) subflow->actual_bid /
        (gdouble) bid_sum *(gdouble) SCHTREE_MAX_VALUE;
    insert_value = (gint) roundf (actual_value);
    _schtree_insert (&new_root, path, &insert_value, SCHTREE_MAX_VALUE);
  }


  //check 128 integrity
  _schnode_overlap_trees (this->full_tree, new_root);
  _schnode_rdtor (this->full_tree);
  this->full_tree = new_root;
  _schtree_loadup (this->full_tree, path_with_largest_bid);
}


void
_keyframes_tree_commit (StreamSplitter * this, guint32 bid_sum,
    MPRTPSPathState filter)
{
  gdouble actual_value;
  gint insert_value;
  SchNode *new_root = NULL;
  MPRTPSPath *path, *path_with_largest_bid = NULL;
  guint32 largest_bid = 0;
  Subflow *subflow;
  gpointer val, key;
  GHashTableIter iter;
  //guint8 subflow_id;

  g_hash_table_iter_init (&iter, this->subflows);
  while (g_hash_table_iter_next (&iter, (gpointer) & key, (gpointer) & val)) {
    subflow = (Subflow *) val;
    path = subflow->path;

    if (mprtps_path_get_state (path) != filter) {
      continue;
    }
    if (largest_bid < subflow->actual_bid) {
      largest_bid = subflow->actual_bid;
      path_with_largest_bid = path;
    }
    actual_value =
        (gdouble) subflow->actual_bid /
        (gdouble) bid_sum *(gdouble) SCHTREE_MAX_VALUE;
    insert_value = (gint) roundf (actual_value);
    _schtree_insert (&new_root, path, &insert_value, SCHTREE_MAX_VALUE);
  }


  //check 128 integrity
  _schnode_overlap_trees (this->keyframes_tree, new_root);
  _schnode_rdtor (this->keyframes_tree);
  this->keyframes_tree = new_root;
  _schtree_loadup (this->keyframes_tree, path_with_largest_bid);
}



void
_schnode_rdtor (SchNode * node)
{
  if (node == NULL) {
    return;
  }
  _schnode_rdtor (node->left);
  _schnode_rdtor (node->right);
  g_free (node);
}

void
_schnode_overlap_trees (SchNode * old_node, SchNode * new_node)
{
  if (old_node == NULL || new_node == NULL) {
    return;
  }
  if (old_node->path != NULL || new_node->path != NULL) {
    return;
  }
  if (old_node->next == old_node->right) {
    new_node->next = new_node->right;
  }
  _schnode_overlap_trees (old_node->left, new_node->left);
  _schnode_overlap_trees (old_node->right, new_node->right);
}


void
_schtree_insert (SchNode ** node, MPRTPSPath * path, gint * change,
    gint level_value)
{
  if (*node == NULL) {
    *node = _schnode_ctor ();
  }
  if ((*node)->path != NULL || level_value < 1) {
    return;
  }
  if (*change >= level_value && (*node)->left == NULL && (*node)->right == NULL) {
    *change -= level_value;
    (*node)->path = path;
    return;
  }

  _schtree_insert (&(*node)->left, path, change, level_value >> 1);
  if (*change < 1) {
    return;
  }
  _schtree_insert (&(*node)->right, path, change, level_value >> 1);
}

SchNode *
_schnode_ctor (void)
{
  SchNode *result = (SchNode *) g_malloc0 (sizeof (SchNode));
  result->left = NULL;
  result->right = NULL;
  result->next = NULL;
  result->path = NULL;
  return result;
}

MPRTPSPath *
schtree_get_next (SchNode * root)
{
  MPRTPSPath *result;
  SchNode *selected;
  selected = root;
  while (selected->path == NULL) {
    selected->next =
        (selected->next == selected->left) ? selected->right : selected->left;
    selected = selected->next;
  }
  result = selected->path;
  return result;
}


Subflow *
make_subflow (MPRTPSPath * path)
{
  Subflow *result = g_malloc0 (sizeof (Subflow));
  result->path = path;
  return result;
}

#undef THIS_READLOCK
#undef THIS_READUNLOCK
#undef THIS_WRITELOCK
#undef THIS_WRITEUNLOCK
#undef SCHTREE_MAX_VALUE
