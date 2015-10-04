/*
 * mprtpssubflow.h
 *
 *  Created on: Jun 30, 2015
 *      Author: balazs
 */

#ifndef MPRTPRSUBFLOW_H_
#define MPRTPRSUBFLOW_H_

#include <gst/gst.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include "gstmprtcpbuffer.h"

G_BEGIN_DECLS typedef struct _MPRTPReceiverPath MPRTPRPath;
typedef struct _MPRTPReceiverPathClass MPRTPRPathClass;


#define MPRTPR_PATH_TYPE             (mprtpr_path_get_type())
#define MPRTPR_PATH(src)             (G_TYPE_CHECK_INSTANCE_CAST((src),MPRTPR_PATH_TYPE,MPRTPRPath))
#define MPRTPR_PATH_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass),MPRTPR_PATH_TYPE,MPRTPRPathClass))
#define MPRTPR_PATH_IS_SOURCE(src)          (G_TYPE_CHECK_INSTANCE_TYPE((src),MPRTPR_PATH_TYPE))
#define MPRTPR_PATH_IS_SOURCE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass),MPRTPR_PATH_TYPE))
#define MPRTPR_PATH_CAST(src)        ((MPRTPRPath *)(src))



struct _MPRTPReceiverPath
{
  GObject       object;
  guint8        id;
  GRWLock       rwmutex;

  GList*        gaps;
  GList*        result;
  guint8        ext_header_id;
  gboolean      seq_initialized;
  gboolean      skew_initialized;
  guint16       cycle_num;
  guint32       jitter;

  guint32       received_since_cycle_is_increased;
  guint32       total_late_discarded;
  guint32       total_late_discarded_bytes;
  guint32       total_early_discarded;
  guint32       total_duplicated_packet_num;
  guint16       actual_seq;

  guint64       ext_rtptime;
  guint64       last_packet_skew;
  guint64       skews[100];
  guint64       received_times[100];
  guint8        skews_write_index;
  guint8        skews_read_index;
  GstClockTime  last_received_time;
  GstClock*     sysclock;
  guint16       HSN;
  guint32       total_packet_losts;
  guint32       packet_received;
  guint64       total_packet_received;
//  guint         received_payload_bytes;
//  gfloat        avg_rtp_size;

};

struct _MPRTPReceiverPathClass
{
  GObjectClass parent_class;
};


GType mprtpr_path_get_type (void);
MPRTPRPath *make_mprtpr_path (guint8 id);
void mprtpr_path_process_mprtp_packet (MPRTPRPath * this, GstBuffer *buf, guint16 seq);
void mprtpr_path_add_packet_skew (MPRTPRPath * this, guint32 rtptime, guint32 clockrate);
guint64 mprtpr_path_get_packet_skew_median (MPRTPRPath * this);

guint16 mprtpr_path_get_cycle_num(MPRTPRPath * this);
guint16 mprtpr_path_get_highest_sequence_number(MPRTPRPath * this);
guint32 mprtpr_path_get_jitter(MPRTPRPath * this);
guint32 mprtpr_path_get_total_packet_losts_num (MPRTPRPath * this);
guint32 mprtpr_path_get_total_late_discarded_num(MPRTPRPath * this);
guint32 mprtpr_path_get_total_late_discarded_bytes_num(MPRTPRPath * this);
guint32 mprtpr_path_get_total_duplicated_packet_num(MPRTPRPath * this);
guint64 mprtpr_path_get_total_received_packets_num (MPRTPRPath * this);
GList *mprtpr_path_get_packets (MPRTPRPath * this);
guint32 mprtpr_path_get_total_early_discarded_packets_num (MPRTPRPath * this);
guint8 mprtpr_path_get_id (MPRTPRPath * this);


G_END_DECLS
#endif /* MPRTPRSUBFLOW_H_ */
