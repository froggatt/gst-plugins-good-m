
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

G_DEFINE_TYPE (MPRTPRPath, mprtpr_path, G_TYPE_OBJECT);


#define THIS_READLOCK(this) g_rw_lock_reader_lock(&this->rwmutex)
#define THIS_READUNLOCK(this) g_rw_lock_reader_unlock(&this->rwmutex)
#define THIS_WRITELOCK(this) g_rw_lock_writer_lock(&this->rwmutex)
#define THIS_WRITEUNLOCK(this) g_rw_lock_writer_unlock(&this->rwmutex)

typedef struct _Gap
{
  GList *at;
  guint16 start;
  guint16 end;
  guint16 total;
  guint16 filled;
} Gap;

static void mprtpr_path_finalize (GObject * object);
static void mprtpr_path_reset (MPRTPRPath * this);

static guint16
_mprtp_buffer_get_sequence_num (GstRTPBuffer * rtp, guint8 MPRTP_EXT_HEADER_ID);

static gboolean
_found_in_gaps (GList * gaps, guint16 actual_subflow_sequence,
    guint8 ext_header_id, GList ** result_item, Gap ** result_gap);
static Gap *_make_gap (GList * at, guint16 start, guint16 end);
static gint _cmp_seq (guint16 x, guint16 y);


void
mprtpr_path_class_init (MPRTPRPathClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = mprtpr_path_finalize;

  GST_DEBUG_CATEGORY_INIT (gst_mprtpr_path_debug_category, "mprtpr_path",
      0, "MPRTP Receiver Subflow");
}


MPRTPRPath *
make_mprtpr_path (guint8 id)
{
  MPRTPRPath *result;

  result = g_object_new (MPRTPR_PATH_TYPE, NULL);
  THIS_WRITELOCK (result);
  result->id = id;
  THIS_WRITEUNLOCK (result);
  return result;
}

void
mprtpr_path_reset (MPRTPRPath * this)
{
  this->gaps = NULL;
  this->result = NULL;
  this->seq_initialized = FALSE;
  this->skew_initialized = FALSE;
  this->cycle_num = 0;
  this->jitter = 0;
  this->received_since_cycle_is_increased = 0;
  this->total_late_discarded = 0;
  this->total_late_discarded_bytes = 0;
  this->total_early_discarded = 0;
  this->total_duplicated_packet_num = 0;
  this->actual_seq = 0;

  this->ext_rtptime = 0;
  this->last_packet_skew = 0;
  this->skews_write_index = 0;
  this->skews_read_index = 0;
  this->last_received_time = 0;
  this->HSN = 0;
  this->total_packet_losts = 0;
  this->packet_received = 0;
}

guint16
mprtpr_path_get_cycle_num (MPRTPRPath * this)
{
  guint16 result;
  THIS_READLOCK (this);
  result = this->cycle_num;
  THIS_READUNLOCK (this);
  return result;
}

guint16
mprtpr_path_get_highest_sequence_number (MPRTPRPath * this)
{
  guint16 result;
  THIS_READLOCK (this);
  result = this->actual_seq;
  THIS_READUNLOCK (this);
  return result;
}

guint32
mprtpr_path_get_jitter (MPRTPRPath * this)
{
  guint32 result;
  THIS_READLOCK (this);
  result = this->jitter;
  THIS_READUNLOCK (this);
  return result;
}

guint32
mprtpr_path_get_total_packet_losts_num (MPRTPRPath * this)
{
  guint16 result;
  THIS_READLOCK (this);
  result = this->total_packet_losts;
  THIS_READUNLOCK (this);
  return result;
}

guint32
mprtpr_path_get_total_late_discarded_num (MPRTPRPath * this)
{
  guint16 result;
  THIS_READLOCK (this);
  result = this->total_late_discarded;
  THIS_READUNLOCK (this);
  return result;
}

guint32
mprtpr_path_get_total_late_discarded_bytes_num (MPRTPRPath * this)
{
  guint32 result;
  THIS_READLOCK (this);
  result = this->total_late_discarded_bytes;
  THIS_READUNLOCK (this);
  return result;
}

guint64
mprtpr_path_get_total_received_packets_num (MPRTPRPath * this)
{
  guint32 result;
  THIS_READLOCK (this);
  result = this->total_packet_received;
  THIS_READUNLOCK (this);
  return result;
}


guint32
mprtpr_path_get_total_duplicated_packet_num (MPRTPRPath * this)
{
  guint16 result;
  THIS_READLOCK (this);
  result = this->total_duplicated_packet_num;
  THIS_READUNLOCK (this);
  return result;
}

void
mprtpr_path_init (MPRTPRPath * this)
{
  g_rw_lock_init (&this->rwmutex);
  this->sysclock = gst_system_clock_obtain ();
  this->ext_header_id = MPRTP_DEFAULT_EXTENSION_HEADER_ID;
  mprtpr_path_reset (this);
}


void
mprtpr_path_finalize (GObject * object)
{
  MPRTPRPath *this;
  this = MPRTPR_PATH_CAST (object);
  g_object_unref (this->sysclock);

}

GList *
mprtpr_path_get_packets (MPRTPRPath * this)
{
  GList *result, *it;
  Gap *gap;
  THIS_WRITELOCK (this);

  result = this->result;
  for (it = this->gaps; it != NULL; it = it->next) {
    gap = it->data;
    if (gap->filled > 0) {
      ++this->total_early_discarded;
    }
    this->total_packet_losts += gap->total - gap->filled;
  }

  g_list_free_full (this->gaps, g_free);
  this->gaps = NULL;
  this->result = NULL;
  this->packet_received = 0;

  THIS_WRITEUNLOCK (this);
  return result;
}


guint32
mprtpr_path_get_total_early_discarded_packets_num (MPRTPRPath * this)
{
  guint32 result;
  THIS_READLOCK (this);
  result = this->total_early_discarded;
  THIS_READUNLOCK (this);
  return result;
}


guint8
mprtpr_path_get_id (MPRTPRPath * this)
{
  guint8 result;
  THIS_READLOCK (this);
  result = this->id;
  THIS_READUNLOCK (this);
  return result;
}


static gint
_cmp_guint64 (gconstpointer a, gconstpointer b, gpointer user_data)
{
  const guint64 *_a = a, *_b = b;
  if (*_a == *_b) {
    return 0;
  }
  return *_a < *_b ? -1 : 1;
}

guint64
mprtpr_path_get_packet_skew_median (MPRTPRPath * this)
{
  guint64 skews[100];
  gint c;
  GstClockTime treshold;
  guint8 i;
  guint64 result;

  THIS_WRITELOCK (this);
  if (this->skews_read_index == this->skews_write_index) {
    result = 0;
    goto done;
  }
  treshold = gst_clock_get_time (this->sysclock) - 2 * GST_SECOND;
  for (c = 0; this->skews_read_index != this->skews_write_index;) {
    i = this->skews_read_index;
    if (++this->skews_read_index == 100) {
      this->skews_read_index = 0;
    }
    if (this->received_times[i] < treshold) {
      continue;
    }
    skews[c++] = this->skews[i];
  }
  this->skews_read_index = this->skews_write_index = 0;
  g_qsort_with_data (skews, c, sizeof (guint64), _cmp_guint64, NULL);
  result = skews[c >> 1];

done:
  THIS_WRITEUNLOCK (this);
  return result;
}

void
mprtpr_path_add_packet_skew (MPRTPRPath * this,
    guint32 rtptime, guint32 clockrate)
{
  guint64 packet_skew, send_diff, recv_diff, last_ext_rtptime;
  guint64 received;
  THIS_WRITELOCK (this);

  received = gst_clock_get_time (this->sysclock);
  if (this->skew_initialized == FALSE) {
    this->ext_rtptime =
        gst_rtp_buffer_ext_timestamp (&this->ext_rtptime, rtptime);
    this->last_received_time = received;
    this->skew_initialized = TRUE;
    goto done;
  }
  last_ext_rtptime = this->ext_rtptime;
  this->ext_rtptime =
      gst_rtp_buffer_ext_timestamp (&this->ext_rtptime, rtptime);

  recv_diff = received - this->last_received_time;
  if (recv_diff > 0x8000000000000000) {
    recv_diff = 0;
  }

  send_diff = gst_util_uint64_scale_int (this->ext_rtptime - last_ext_rtptime,
      GST_SECOND, clockrate);

  if (send_diff > 0x8000000000000000) {
    send_diff = 0;
  }
  if (send_diff == 0) {
    goto done;
  }
  packet_skew = recv_diff - send_diff;
  if (packet_skew > 0x8000000000000000) {
    packet_skew = this->last_packet_skew;
  }

  this->jitter = this->jitter +
      (((gfloat) packet_skew - (gfloat) this->jitter) / 16.);

  this->last_packet_skew = packet_skew;
  this->skews[this->skews_write_index] = this->last_packet_skew;

  this->received_times[this->skews_write_index] = received;

  if (++this->skews_write_index == 100) {
    this->skews_write_index = 0;
  }

  if (this->skews_write_index == this->skews_read_index) {
    if (++this->skews_read_index == 100) {
      this->skews_read_index = 0;
    }
  }

  this->ext_rtptime =
      gst_rtp_buffer_ext_timestamp (&this->ext_rtptime, rtptime);
  //this->last_sent_time = sent;
  this->last_received_time = received;

done:
  THIS_WRITEUNLOCK (this);
}


void
mprtpr_path_process_mprtp_packet (MPRTPRPath * this, GstBuffer * buf,
    guint16 subflow_sequence)
{
  GList *it;
  Gap *gap;
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  guint payload_size;
  THIS_WRITELOCK (this);
//  printf ("Packet is received by %d path receiver "
//      "with %d relative sequence\n", this->id, subflow_sequence);

  if (this->seq_initialized == FALSE) {
    this->actual_seq = subflow_sequence;
    this->HSN = subflow_sequence;
    this->packet_received = 1;
    this->total_packet_received = 1;
    this->seq_initialized = TRUE;
    this->result = g_list_prepend (this->result, buf);
    goto done;
  }
  //goto mprtpr_path_process_rtpbuffer_end;

  //calculate lost, discarded and received packets
  ++this->packet_received;
  ++this->total_packet_received;
  if (0x8000 < this->HSN && subflow_sequence < 0x8000 &&
      this->received_since_cycle_is_increased > 0x8888) {
    this->received_since_cycle_is_increased = 0;
    ++this->cycle_num;
  }

  if (_cmp_seq (this->HSN, subflow_sequence) > 0) {
    ++this->total_late_discarded;
    gst_rtp_buffer_map (buf, GST_MAP_READ, &rtp);
    payload_size = gst_rtp_buffer_get_payload_len (&rtp);
    this->total_late_discarded_bytes += payload_size;
    gst_rtp_buffer_unmap (&rtp);
    goto done;
  }
  if (subflow_sequence == (guint16) (this->actual_seq + 1)) {
    ++this->received_since_cycle_is_increased;
    this->result = g_list_prepend (this->result, buf);
    ++this->actual_seq;
    goto done;
  }
  if (_cmp_seq (this->actual_seq, subflow_sequence) < 0) {      //GAP
    this->result = g_list_prepend (this->result, buf);
    gap = _make_gap (this->result, this->actual_seq, subflow_sequence);
    this->gaps = g_list_append (this->gaps, gap);
    this->actual_seq = subflow_sequence;
    goto done;
  }
  if (_cmp_seq (this->actual_seq, subflow_sequence) > 0 && _found_in_gaps (this->gaps, subflow_sequence, this->ext_header_id, &it, &gap) == TRUE) {     //Discarded
    this->result =
        g_list_insert_before (this->result, it != NULL ? it : gap->at, buf);
    //this->result = dlist_pre_insert(this->result, it != NULL ? it : gap->at, rtp);
    ++gap->filled;
    goto done;
  }
  ++this->total_duplicated_packet_num;

done:
  THIS_WRITEUNLOCK (this);
  return;
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

Gap *
_make_gap (GList * at, guint16 start, guint16 end)
{
  Gap *result = g_new0 (Gap, 1);
  guint16 counter;

  result->at = at;
  result->start = start;
  result->end = end;
  //_mprtp_buffer_get_sequence_num(at->data, &result->end);
  //result->active = BOOL_TRUE;
  result->total = 1;
  for (counter = result->start + 1;
      counter != (guint16) (result->end - 1); ++counter, ++result->total);
  //printf("Make Gap: start: %d - end: %d, missing: %d\n",result->start, result->end, result->total);
  return result;
}

gboolean
_found_in_gaps (GList * gaps,
    guint16 actual_subflow_sequence,
    guint8 ext_header_id, GList ** result_item, Gap ** result_gap)
{

  Gap *gap;
  GList *it;
  GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
  GstBuffer *buf;
  gint32 cmp;
  guint16 rtp_subflow_sequence;
  for (it = gaps; it != NULL; it = it->next) {
    gap = (Gap *) it->data;
    /*
       printf("\nGap total: %d; Filled: %d Start seq:%d End seq:%d\n",
       gap->total, gap->filled, gap->start, gap->end);
     */
    //if(gap->active == BOOL_FALSE){
    if (gap->filled == gap->total) {
      continue;
    }
    if (_cmp_seq (gap->start, actual_subflow_sequence) <= 0
        && _cmp_seq (actual_subflow_sequence, gap->end) <= 0) {
      break;
    }

  }
  if (it == NULL) {
    return FALSE;
  }
  if (result_gap != NULL) {
    *result_gap = gap;
  }

  for (it = gap->at; it != NULL; it = it->next) {
    //rtp = it->data;
    buf = it->data;
    gst_rtp_buffer_map (buf, GST_MAP_READ, &rtp);
    rtp_subflow_sequence = _mprtp_buffer_get_sequence_num (&rtp, ext_header_id);
    cmp = _cmp_seq (rtp_subflow_sequence, actual_subflow_sequence);
    gst_rtp_buffer_unmap (&rtp);
    //printf("packet_s: %d, actual_s: %d\n",packet->sequence, actual->sequence);
    if (cmp > 0) {
      continue;
    }
    if (cmp == 0) {
      return FALSE;
    }
    break;
  }
  if (result_item != NULL) {
    *result_item = it;
    //printf("result_item: %d",((packet_t*)it->next->data)->sequence);
  }
  return TRUE;
}


guint16
_mprtp_buffer_get_sequence_num (GstRTPBuffer * rtp, guint8 MPRTP_EXT_HEADER_ID)
{
  gpointer pointer = NULL;
  guint size = 0;
  MPRTPSubflowHeaderExtension *ext_header;
  if (!gst_rtp_buffer_get_extension_onebyte_header (rtp, MPRTP_EXT_HEADER_ID, 0,
          &pointer, &size)) {
    GST_WARNING
        ("The requested rtp buffer doesn't contain one byte header extension with id: %d",
        MPRTP_EXT_HEADER_ID);
    return FALSE;
  }
  ext_header = (MPRTPSubflowHeaderExtension *) pointer;
  return ext_header->seq;
}

#undef THIS_READLOCK
#undef THIS_READUNLOCK
#undef THIS_WRITELOCK
#undef THIS_WRITEUNLOCK
