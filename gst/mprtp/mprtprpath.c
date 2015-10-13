
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include "mprtprpath.h"
#include "mprtpspath.h"
#include "gstmprtcpbuffer.h"

GST_DEBUG_CATEGORY_STATIC (gst_mprtpr_path_debug_category);
#define GST_CAT_DEFAULT gst_mprtpr_path_debug_category

G_DEFINE_TYPE (MpRTPRPath, mprtpr_path, G_TYPE_OBJECT);


#define THIS_READLOCK(this) g_rw_lock_reader_lock(&this->rwmutex)
#define THIS_READUNLOCK(this) g_rw_lock_reader_unlock(&this->rwmutex)
#define THIS_WRITELOCK(this) g_rw_lock_writer_lock(&this->rwmutex)
#define THIS_WRITEUNLOCK(this) g_rw_lock_writer_unlock(&this->rwmutex)

#define GET_SKEW_TREE_TOP(tree) (tree->top)
#define GET_SKEW_TREE_NUM(tree) (tree->counter)
#define _skew_tree_remove_top(tree) _remove_skew_tree(tree, tree->top)

typedef struct _Gap Gap;

struct _Gap
{
  MpRTPPacket *at;
  guint16 start;
  guint16 end;
  guint16 total;
  guint16 filled;
};

struct _SkewTree
{
  MpRTPPacket *root;
  MpRTPPacket *top;
    gint (*cmp) (MpRTPPacket *, MpRTPPacket *);
  gint counter;
};

struct _SkewChain
{
  MpRTPPacket *head, *tail, *play;
  gint counter;
};


struct _MpRTPPacket
{
  GstBuffer *buffer;
  GstClockTime arrival;
  GstClockTimeDiff interleave;
  MpRTPPacket *next;
  MpRTPPacket *successor;
  MpRTPPacket *left;
  MpRTPPacket *right;
  MpRTPPacket *parent;
  SkewTree *tree;
  Gap *gap;
  guint32 payload_bytes;
  guint16 abs_seq_num;
  guint16 rel_seq_num;
  GstClockTime rcv_time;
  GstClockTime snd_time;
  GstClockTimeDiff delay;
  guint64 skew;

};

static void mprtpr_path_finalize (GObject * object);
static void mprtpr_path_reset (MpRTPRPath * this);

static MpRTPPacket *_make_packet (MpRTPRPath * this,
    GstRTPBuffer * rtp, guint16 packet_subflow_seq_num, GstClockTime snd_time);
static void _trash_packet (MpRTPRPath * this, MpRTPPacket * packet);
static void _add_packet (MpRTPRPath * this, MpRTPPacket * packet);
static void
_chain_packets (MpRTPRPath * this, MpRTPPacket * actual, MpRTPPacket * next);
static void _ruin_packet_skew_tree (MpRTPRPath * this, SkewTree * tree);
static void
_reset_skew_trees (MpRTPRPath * this, MpRTPPacket * first,
    MpRTPPacket * second);
static void _add_packet_skew (MpRTPRPath * this, MpRTPPacket * packet);
static void _balancing_skew_tree (MpRTPRPath * this);
static MpRTPPacket *_insert_skew_tree (SkewTree * tree, MpRTPPacket * packet);
static MpRTPPacket *_remove_skew_tree (SkewTree * tree, MpRTPPacket * packet);
static void
_make_gap (MpRTPRPath * this, MpRTPPacket * at, guint16 start, guint16 end);
static void _trash_gap (MpRTPRPath * this, Gap * gap);
static gboolean _try_fill_a_gap (MpRTPRPath * this, MpRTPPacket * packet);
static gint _cmp_seq (guint16 x, guint16 y);
static gint _cmp_for_max (MpRTPPacket * x, MpRTPPacket * y);
static gint _cmp_for_min (MpRTPPacket * x, MpRTPPacket * y);

void static
_print_tree (SkewTree * tree, MpRTPPacket * node, gint level)
{
  gint i;
  if (node == NULL) {
    return;
  }
  if (!level)
    g_print ("Tree %p TOP is: %p \n", tree, tree->top);
  for (i = 0; i < level && i < 10; ++i)
    g_print ("--");
  g_print ("%d->%p->skew:%lu ->parent: %p ->left:%p ->right:%p\n",
      level, node, node->skew, node->parent, node->left, node->right);
  _print_tree (tree, node->left, level + 1);
  _print_tree (tree, node->right, level + 1);
}


void
mprtpr_path_class_init (MpRTPRPathClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = mprtpr_path_finalize;

  GST_DEBUG_CATEGORY_INIT (gst_mprtpr_path_debug_category, "mprtpr_path",
      0, "MPRTP Receiver Subflow");
}


MpRTPRPath *
make_mprtpr_path (guint8 id)
{
  MpRTPRPath *result;

  result = g_object_new (MPRTPR_PATH_TYPE, NULL);
  THIS_WRITELOCK (result);
  result->id = id;
  THIS_WRITEUNLOCK (result);
  return result;
}


void
mprtpr_path_init (MpRTPRPath * this)
{
  g_rw_lock_init (&this->rwmutex);
  this->sysclock = gst_system_clock_obtain ();
  this->packet_chain = (PacketChain *) g_malloc0 (sizeof (PacketChain));
  this->packets_pool = g_queue_new ();
  this->gaps_pool = g_queue_new ();
  this->skew_max_tree = g_malloc0 (sizeof (SkewTree));
  this->skew_min_tree = g_malloc0 (sizeof (SkewTree));
  this->skew_max_tree->cmp = _cmp_for_max;
  this->skew_min_tree->cmp = _cmp_for_min;

  mprtpr_path_reset (this);
}


void
mprtpr_path_finalize (GObject * object)
{
  MpRTPRPath *this;
  this = MPRTPR_PATH_CAST (object);
  g_object_unref (this->sysclock);
  while (g_queue_is_empty (this->packets_pool))
    g_free (g_queue_pop_head (this->packets_pool));
  while (g_queue_is_empty (this->gaps_pool))
    g_free (g_queue_pop_head (this->gaps_pool));
  g_free (this->skew_max_tree);
  g_free (this->skew_min_tree);

}


void
mprtpr_path_reset (MpRTPRPath * this)
{
  this->gaps = NULL;
  this->result = NULL;
  this->seq_initialized = FALSE;
  //this->skew_initialized = FALSE;
  this->cycle_num = 0;
  this->jitter = 0;
  this->received_since_cycle_is_increased = 0;
  this->total_late_discarded = 0;
  this->total_late_discarded_bytes = 0;
  this->total_early_discarded = 0;
  this->total_duplicated_packet_num = 0;
  this->actual_seq = 0;
  this->total_packet_losts = 0;
  this->total_packet_received = 0;

  this->ext_rtptime = -1;
  this->last_packet_skew = 0;
//  this->skews_write_index = 0;
//  this->skews_read_index = 0;
  this->last_received_time = 0;
  this->HSN = 0;

}

guint16
mprtpr_path_get_cycle_num (MpRTPRPath * this)
{
  guint16 result;
  THIS_READLOCK (this);
  result = this->cycle_num;
  THIS_READUNLOCK (this);
  return result;
}

guint16
mprtpr_path_get_highest_sequence_number (MpRTPRPath * this)
{
  guint16 result;
  THIS_READLOCK (this);
  result = this->actual_seq;
  THIS_READUNLOCK (this);
  return result;
}

guint32
mprtpr_path_get_jitter (MpRTPRPath * this)
{
  guint32 result;
  THIS_READLOCK (this);
  result = this->jitter;
  THIS_READUNLOCK (this);
  return result;
}

guint32
mprtpr_path_get_total_packet_losts_num (MpRTPRPath * this)
{
  guint16 result;
  THIS_READLOCK (this);
  result = this->total_packet_losts;
  THIS_READUNLOCK (this);
  return result;
}

guint32
mprtpr_path_get_total_late_discarded_num (MpRTPRPath * this)
{
  guint16 result;
  THIS_READLOCK (this);
  result = this->total_late_discarded;
  THIS_READUNLOCK (this);
  return result;
}

guint32
mprtpr_path_get_total_late_discarded_bytes_num (MpRTPRPath * this)
{
  guint32 result;
  THIS_READLOCK (this);
  result = this->total_late_discarded_bytes;
  THIS_READUNLOCK (this);
  return result;
}

guint32
mprtpr_path_get_total_bytes_received (MpRTPRPath * this)
{
  guint32 result;
  THIS_READLOCK (this);
  result = this->total_received_bytes;
  THIS_READUNLOCK (this);
  return result;
}

guint64
mprtpr_path_get_total_received_packets_num (MpRTPRPath * this)
{
  guint32 result;
  THIS_READLOCK (this);
  result = this->total_packet_received;
  THIS_READUNLOCK (this);
  return result;
}


guint32
mprtpr_path_get_total_duplicated_packet_num (MpRTPRPath * this)
{
  guint16 result;
  THIS_READLOCK (this);
  result = this->total_duplicated_packet_num;
  THIS_READUNLOCK (this);
  return result;
}


guint32
mprtpr_path_get_total_early_discarded_packets_num (MpRTPRPath * this)
{
  guint32 result;
  THIS_READLOCK (this);
  result = this->total_early_discarded;
  THIS_READUNLOCK (this);
  return result;
}

guint8
mprtpr_path_get_id (MpRTPRPath * this)
{
  guint8 result;
  THIS_READLOCK (this);
  result = this->id;
  THIS_READUNLOCK (this);
  return result;
}


gboolean
mprtpr_path_has_buffer_to_playout (MpRTPRPath * this)
{
  gboolean result;
  THIS_READLOCK (this);
  result = this->packet_chain->play != NULL;
  THIS_READUNLOCK (this);
  return result;
}

GstBuffer *
mprtpr_path_pop_buffer_to_playout (MpRTPRPath * this, guint16 * abs_seq)
{
  GstBuffer *result = NULL;
  PacketChain *chain;
  MpRTPPacket *packet;
  THIS_WRITELOCK (this);
  chain = this->packet_chain;
  packet = chain->play;
  if (!packet)
    goto done;
  result = packet->buffer;
  packet->buffer = NULL;
  chain->play = packet->successor;
  if (packet->gap) {
    Gap *gap;
    gap = packet->gap;
    this->total_packet_losts += gap->total - gap->filled;
    this->gaps = g_list_remove (this->gaps, gap);
    packet->gap = NULL;
    _trash_gap (this, gap);
  }
  if (abs_seq) {
    *abs_seq = packet->abs_seq_num;
  }
done:
  THIS_WRITEUNLOCK (this);
  return result;
}

void
mprtpr_path_removes_obsolate_packets (MpRTPRPath * this)
{
  MpRTPPacket *actual, *next;
  PacketChain *chain;
  GstClockTime treshold;
  THIS_WRITELOCK (this);
  treshold = gst_clock_get_time (this->sysclock) - 2 * GST_SECOND;
  chain = this->packet_chain;
  if (!chain->head)
    goto done;
  for (actual = chain->head;
      actual && actual != chain->play && actual->rcv_time < treshold;) {
    g_print ("Obsolate packet: %p\n its tree: %p\n", actual, actual->tree);
    if (actual->tree)
      _remove_skew_tree (actual->tree, actual);
    next = actual->next;
    _trash_packet (this, actual);
    chain->head = actual = next;
  }
  _balancing_skew_tree (this);
done:
  THIS_WRITEUNLOCK (this);
}

guint64
mprtpr_path_get_skew (MpRTPRPath * this)
{
  PacketChain *chain;
  guint64 result = 10 * GST_MSECOND;
  SkewTree *min_tree, *max_tree;
  gint max_count, min_count;
  MpRTPPacket *min_top, *max_top;

  THIS_WRITELOCK (this);
  chain = this->packet_chain;
  if (chain->counter < 2)
    goto done;
  if (chain->counter == 2) {
    result = chain->head->next->skew;
    goto done;
  }
  min_tree = this->skew_min_tree;
  max_tree = this->skew_max_tree;
  min_count = GET_SKEW_TREE_NUM (min_tree);
  max_count = GET_SKEW_TREE_NUM (max_tree);
  min_top = GET_SKEW_TREE_TOP (min_tree);
  max_top = GET_SKEW_TREE_TOP (max_tree);

  if (min_count == max_count)
    result = (min_top->skew + max_top->skew) >> 1;
  else if (min_count < max_count)
    result = max_top->skew;
  else
    result = min_top->skew;

done:
  THIS_WRITEUNLOCK (this);
  return result;
}

void
mprtpr_path_process_rtp_packet (MpRTPRPath * this,
    GstRTPBuffer * rtp, guint16 packet_subflow_seq_num, GstClockTime snd_time)
{
  MpRTPPacket *packet;

  THIS_WRITELOCK (this);

  packet = _make_packet (this, rtp, packet_subflow_seq_num, snd_time);

  if (this->seq_initialized == FALSE) {
    this->actual_seq = packet_subflow_seq_num;
    this->HSN = packet_subflow_seq_num;
    this->total_packet_received = 1;
    this->seq_initialized = TRUE;
    //this->result = g_list_prepend (this->result, buf);
    _add_packet (this, packet);
    goto done;
  }
  //goto mprtpr_path_process_rtpbuffer_end;

  //calculate lost, discarded and received packets
  ++this->total_packet_received;
  if (0x8000 < this->HSN && packet_subflow_seq_num < 0x8000 &&
      this->received_since_cycle_is_increased > 0x8888) {
    this->received_since_cycle_is_increased = 0;
    ++this->cycle_num;
  }

  if (_cmp_seq (this->HSN, packet_subflow_seq_num) > 0) {
    ++this->total_late_discarded;
    this->total_late_discarded_bytes += packet->payload_bytes;
    goto done;
  }

  _add_packet (this, packet);

  this->total_received_bytes += packet->payload_bytes + (28 << 3);
  if (packet_subflow_seq_num == (guint16) (this->actual_seq + 1)) {
    ++this->received_since_cycle_is_increased;
    ++this->actual_seq;
    goto done;
  }
  if (_cmp_seq (this->actual_seq, packet_subflow_seq_num) < 0) {        //GAP
    _make_gap (this, packet, this->actual_seq, packet_subflow_seq_num);
    this->actual_seq = packet_subflow_seq_num;
    goto done;
  }
  if (_cmp_seq (this->actual_seq, packet_subflow_seq_num) > 0) {
    if (_try_fill_a_gap (this, packet))
      goto done;
  }
  ++this->total_duplicated_packet_num;
  gst_buffer_unref (packet->buffer);
  packet->buffer = NULL;

done:
  THIS_WRITEUNLOCK (this);
  return;

}

MpRTPPacket *
_make_packet (MpRTPRPath * this,
    GstRTPBuffer * rtp, guint16 packet_subflow_seq_num, GstClockTime snd_time)
{
  MpRTPPacket *result;
  guint32 rtptime;
  if (g_queue_is_empty (this->packets_pool)) {
    result = (MpRTPPacket *) g_malloc0 (sizeof (MpRTPPacket));
  } else {
    result = g_queue_pop_head (this->packets_pool);
  }

  rtptime = gst_rtp_buffer_get_timestamp (rtp);
  this->ext_rtptime =
      gst_rtp_buffer_ext_timestamp (&this->ext_rtptime, rtptime);

  result->buffer = rtp->buffer;
  gst_buffer_ref (result->buffer);
  result->left = NULL;
  result->gap = NULL;
  result->right = NULL;
  result->parent = NULL;
  result->next = NULL;
  result->successor = NULL;
  result->tree = NULL;
  result->payload_bytes = gst_rtp_buffer_get_payload_len (rtp);
  result->abs_seq_num = gst_rtp_buffer_get_seq (rtp);
  result->rel_seq_num = packet_subflow_seq_num;
  result->rcv_time = gst_clock_get_time (this->sysclock);
  result->snd_time = snd_time;

  result->delay = GST_CLOCK_DIFF (result->snd_time, result->rcv_time);
//
//  g_print("Packet->buf: %p\n"
//          "Packet->payload_bytes: %u\n"
//          "Packet->abs_seq: %hu\n"
//          "Packet->rel_seq: %hu\n"
//          "Packet->rcv_time: %lu\n"
//          "Packet->snd_time: %lu\n", result->buffer, result->payload_bytes,
//          result->abs_seq_num, result->rel_seq_num, result->rcv_time,
//          result->snd_time);

  return result;
}

void
_trash_packet (MpRTPRPath * this, MpRTPPacket * packet)
{
  if (g_queue_get_length (this->packets_pool) < 256)
    g_queue_push_head (this->packets_pool, packet);
  else
    g_free (packet);
}

void
_add_packet (MpRTPRPath * this, MpRTPPacket * packet)
{
  PacketChain *chain;
  chain = this->packet_chain;
  if (!chain->head) {
    chain->head = chain->play = chain->tail = packet;
    goto done;
  }

  _chain_packets (this, chain->tail, packet);
  chain->tail = chain->tail->successor = packet;

  if (!chain->play)
    chain->play = packet;
  if (chain->counter < 3) {
    goto done;
  } else if (chain->counter == 3) {
    _reset_skew_trees (this, chain->head->next, chain->head->next->next);
    goto done;
  }
  //calculate everything
  _add_packet_skew (this, packet);

done:
  ++chain->counter;
  return;
}


void
_chain_packets (MpRTPRPath * this, MpRTPPacket * actual, MpRTPPacket * next)
{
  guint64 snd_diff, rcv_diff;
  //guint64 received;

  rcv_diff = next->rcv_time - actual->rcv_time;
  if (rcv_diff > 0x8000000000000000) {
    rcv_diff = 0;
  }

  snd_diff = next->snd_time - actual->snd_time;
  if (snd_diff > 0x8000000000000000) {
    snd_diff = 0;
  }

  if (snd_diff == 0) {
    GST_WARNING_OBJECT (this, "The skew between two packets NOT real");
    goto done;
  }

  if (rcv_diff < snd_diff)
    next->skew = snd_diff - rcv_diff;
  else
    next->skew = rcv_diff - snd_diff;

//  g_print("RCV_DIFF: %lu SND_DIFF: %lu PACKET SKEW: %lu\n",
//          rcv_diff, snd_diff, next->skew);
  if (next->skew > 0x8000000000000000) {
    GST_WARNING_OBJECT (this, "The skew between two packets NOT real");
    next->skew = 0;
    goto done;
  }

  this->jitter = this->jitter +
      (((gfloat) next->skew - (gfloat) this->jitter) / 16.);

done:
  actual->next = next;
  return;
}

void
_ruin_packet_skew_tree (MpRTPRPath * this, SkewTree * tree)
{
  MpRTPPacket *packet;
  packet = tree->root;
  while (packet) {
    packet = _remove_skew_tree (tree, packet);
    _trash_packet (this, packet);
  }
  tree->root = NULL;
  tree->counter = 0;
  tree->top = NULL;
}



void
_reset_skew_trees (MpRTPRPath * this, MpRTPPacket * first, MpRTPPacket * second)
{
  //make sure there is nothing in the trees
  if (this->skew_max_tree)
    _ruin_packet_skew_tree (this, this->skew_max_tree);
  if (this->skew_min_tree)
    _ruin_packet_skew_tree (this, this->skew_min_tree);

  if (first->skew < second->skew) {
    _insert_skew_tree (this->skew_max_tree, first);
    _insert_skew_tree (this->skew_min_tree, second);
  } else {
    _insert_skew_tree (this->skew_min_tree, first);
    _insert_skew_tree (this->skew_max_tree, second);
  }
}

void
_add_packet_skew (MpRTPRPath * this, MpRTPPacket * packet)
{
  SkewTree **tree;

  tree = (GET_SKEW_TREE_TOP (this->skew_max_tree)->skew <
      packet->skew) ? &this->skew_max_tree : &this->skew_min_tree;
  (*tree)->root = _insert_skew_tree (*tree, packet);

  _balancing_skew_tree (this);

}

void
_balancing_skew_tree (MpRTPRPath * this)
{
  gint min_tree_num;
  gint max_tree_num;
  gint diff;
  MpRTPPacket *packet;

balancing:
  min_tree_num = GET_SKEW_TREE_NUM (this->skew_min_tree);
  max_tree_num = GET_SKEW_TREE_NUM (this->skew_max_tree);
  diff = max_tree_num - min_tree_num;
//  g_print("max_tree_num: %d, min_tree_num: %d\n", max_tree_num, min_tree_num);
  if (-2 < diff && diff < 2) {
    goto done;
  }
  if (diff < -1) {
    packet = GET_SKEW_TREE_TOP (this->skew_min_tree);
    this->skew_min_tree->root = _skew_tree_remove_top (this->skew_min_tree);

    g_print
        ("Balancing. From min to max packet: %p ->parent: %p ->left: %p ->right: %p\n",
        packet, packet->parent, packet->left, packet->right);
    this->skew_max_tree->root = _insert_skew_tree (this->skew_max_tree, packet);
  } else if (1 < diff) {
    packet = GET_SKEW_TREE_TOP (this->skew_max_tree);
    this->skew_max_tree->root = _skew_tree_remove_top (this->skew_max_tree);

    g_print
        ("Balancing. From max to min packet: %p ->parent: %p ->left: %p ->right: %p\n",
        packet, packet->parent, packet->left, packet->right);
    this->skew_min_tree->root = _insert_skew_tree (this->skew_min_tree, packet);
  }
  goto balancing;

done:
  return;
}


MpRTPPacket *
_insert_skew_tree (SkewTree * tree, MpRTPPacket * packet)
{
  MpRTPPacket *node, *parent = NULL;
  g_print ("BEFORE INSERTING %p: \n", packet);
  _print_tree (tree, tree->root, 0);
  if (!tree->root) {
    g_print ("There is no root\n");
    tree->root = tree->top = packet;
    packet->parent = packet->left = packet->right = NULL;
    goto done;
  }
  node = tree->root;
  while (node) {
    parent = node;
    node = tree->cmp (parent, packet) < 0 ? node->right : node->left;
  }
  g_print ("The parent element is: %p ->left: %p ->right:%p\n",
      parent, parent->left, parent->right);
  if (tree->cmp (parent, packet) < 0) {
    parent->right = packet;
    if (tree->cmp (tree->top, packet) < 0) {
      tree->top = packet;
      g_print ("NEW TOP IS SELECTED AT INSERT: %p\n", tree->top);
    }
  } else {
    parent->left = packet;
  }
  packet->parent = parent;
  g_print ("After done the packet %p ->parent: %p ->left: %p ->right :%p\n",
      packet, packet->parent, packet->left, packet->right);
done:
  packet->tree = tree;
  ++tree->counter;
  g_print ("AFTER INSERTING %p: \n", packet);
  _print_tree (tree, tree->root, 0);
  return tree->root;
}

MpRTPPacket *
_remove_skew_tree (SkewTree * tree, MpRTPPacket * packet)
{
  MpRTPPacket *parent = NULL;
  MpRTPPacket *candidate = NULL;
  g_print ("BEFORE REMOVING %p: \n", packet);
  _print_tree (tree, tree->root, 0);
  if (!packet->left && !packet->right) {
    candidate = NULL;
    goto done;
  }
  if (!packet->left) {
    candidate = packet->right;
    goto done;
  } else if (!packet->right) {
    candidate = packet->left;
    goto done;
  }
  for (candidate = packet->left; candidate->right;
      candidate = candidate->right);
  _remove_skew_tree (tree, candidate);

done:
  if (packet == tree->top) {
    MpRTPPacket *right;
    g_print ("%p TOP:%p ->parent:%p, left:%p\n",
        tree, packet, packet->parent, packet->left);
    if (packet->left) {
      tree->top = packet->left;
      for (right = packet->left->right; right;
          tree->top = right, right = right->right);
    } else {
      tree->top = packet->parent;
    }
    g_print ("NEW TOP IS SELECTED AT REMOVE: %p\n", tree->top);
//    g_print("%p removing operation NEW top: %lu\n", tree, tree->top->skew);
  }
  if (candidate)
    candidate->parent = packet->parent;
  if (packet->left)
    packet->left->parent = candidate;
  if (packet->right)
    packet->right->parent = candidate;
  if (!packet->parent) {
    tree->root = candidate;
  } else {
    parent = packet->parent;
    if (parent->left == packet)
      parent->left = candidate;
    else
      parent->right = candidate;
  }
  packet->tree = NULL;
  packet->left = packet->right = packet->parent = NULL;
  --tree->counter;
  g_print ("AFTER REMOVING %p: \n", packet);
  _print_tree (tree, tree->root, 0);
  return tree->root;
}


void
_make_gap (MpRTPRPath * this, MpRTPPacket * at, guint16 start, guint16 end)
{
  Gap *gap;
  guint16 counter;
  if (g_queue_is_empty (this->gaps_pool))
    gap = g_malloc0 (sizeof (Gap));
  else
    gap = (Gap *) g_queue_pop_head (this->gaps_pool);

  gap->at = at;
  gap->start = start;
  gap->end = end;
  gap->filled = 0;
  gap->total = 1;
  for (counter = gap->start + 1;
      counter != (guint16) (gap->end - 1); ++counter, ++gap->total);
  this->gaps = g_list_prepend (this->gaps, gap);
  at->gap = gap;

}

void
_trash_gap (MpRTPRPath * this, Gap * gap)
{
  if (g_queue_get_length (this->gaps_pool) > 100)
    g_free (gap);
  else
    g_queue_push_head (this->gaps_pool, gap);
}


gboolean
_try_fill_a_gap (MpRTPRPath * this, MpRTPPacket * packet)
{
  gboolean result;
  GList *it;
  Gap *gap;
  gint cmp;
  MpRTPPacket *pred;
  MpRTPPacket *succ;

  result = FALSE;
  for (it = this->gaps; it; it = it->next, gap = NULL) {
    gap = it->data;
    cmp = _cmp_seq (gap->start, packet->rel_seq_num);
    if (cmp > 0)
      continue;
    if (cmp == 0)
      goto done;

    cmp = _cmp_seq (packet->rel_seq_num, gap->end);
    if (cmp > 0)
      continue;
    if (cmp == 0)
      goto done;
    break;
  }

  if (!gap)
    goto done;

  for (pred = gap->at; pred->rel_seq_num != gap->end; pred = pred->successor) {
    succ = pred->successor;
    cmp = _cmp_seq (packet->rel_seq_num, succ->rel_seq_num);
    if (cmp > 0)
      break;
    succ = NULL;
  }

  if (!succ)
    goto done;

  pred->successor = packet;
  packet->successor = succ;
  result = TRUE;

done:
  return result;
}


gint
_cmp_seq (guint16 x, guint16 y)
{

  if (x == y) {
    return 0;
  }
  /*
     if(x < y || (0x8000 < x && y < 0x8000)){
     return -1;
     }
     return 1;
   */
  return ((gint16) (x - y)) < 0 ? -1 : 1;

}

gint
_cmp_for_max (MpRTPPacket * x, MpRTPPacket * y)
{
  return x->skew == y->skew ? 0 : x->skew < y->skew ? -1 : 1;
}

gint
_cmp_for_min (MpRTPPacket * x, MpRTPPacket * y)
{
  return x->skew == y->skew ? 0 : x->skew < y->skew ? 1 : -1;
}

#undef GET_SKEW_TREE_TOP
#undef GET_SKEW_TREE_NUM
#undef _skew_tree_remove_top

#undef THIS_READLOCK
#undef THIS_READUNLOCK
#undef THIS_WRITELOCK
#undef THIS_WRITEUNLOCK
