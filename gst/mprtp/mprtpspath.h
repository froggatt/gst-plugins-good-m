/*
 * mprtpssubflow.h
 *
 *  Created on: Jun 30, 2015
 *      Author: balazs
 */

#ifndef MPRTPSPATH_H_
#define MPRTPSPATH_H_

#include <gst/gst.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include "gstmprtcpbuffer.h"

#define MPRTP_DEFAULT_EXTENSION_HEADER_ID 3

G_BEGIN_DECLS typedef struct _MPRTPSPath MPRTPSPath;
typedef struct _MPRTPSPathClass MPRTPSPathClass;
typedef struct _MPRTPSubflowHeaderExtension MPRTPSubflowHeaderExtension;

#define MPRTPS_PATH_TYPE             (mprtps_path_get_type())
#define MPRTPS_PATH(src)             (G_TYPE_CHECK_INSTANCE_CAST((src),MPRTPS_PATH_TYPE,MPRTPSPath))
#define MPRTPS_PATH_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass),MPRTPS_PATH_TYPE,MPRTPSPathClass))
#define MPRTPS_PATH_IS_SOURCE(src)          (G_TYPE_CHECK_INSTANCE_TYPE((src),MPRTPS_PATH_TYPE))
#define MPRTPS_PATH_IS_SOURCE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass),MPRTPS_PATH_TYPE))
#define MPRTPS_PATH_CAST(src)        ((MPRTPSPath *)(src))



struct _MPRTPSubflowHeaderExtension
{
  guint8 id;
  guint16 seq;
};

typedef enum
{
  MPRTPS_PATH_FLAG_NON_LOSSY = 1,
  MPRTPS_PATH_FLAG_NON_CONGESTED = 2,
  MPRTPS_PATH_FLAG_ACTIVE = 4,
} MPRTPSubflowFlags;

typedef enum{
  MPRTPS_PATH_STATE_NON_CONGESTED,
  MPRTPS_PATH_STATE_MIDDLY_CONGESTED,
  MPRTPS_PATH_STATE_CONGESTED,
  MPRTPS_PATH_STATE_PASSIVE,
}MPRTPSPathState;


struct _MPRTPSPath
{
  GObject   object;

  GRWLock   rwmutex;
  gboolean  is_new;
  GstClock* sysclock;
  guint8    id;
  guint16   seq;
  guint16   cycle_num;
  guint8    state;
  guint32   sent_packet_num;
  guint32   total_sent_packet_num;
  guint32   sent_payload_bytes_sum;
  guint32   total_sent_payload_bytes_sum;

  GstClockTime  sent_passive;
  GstClockTime  sent_active;
};

struct _MPRTPSPathClass
{
  GObjectClass parent_class;
};



GType mprtps_path_get_type (void);
MPRTPSPath *make_mprtps_path (guint8 id);

gboolean mprtps_path_is_new (MPRTPSPath * this);
void mprtps_path_set_not_new(MPRTPSPath * this);
gboolean mprtps_path_is_active (MPRTPSPath * this);
void mprtps_path_set_active (MPRTPSPath * this);
void mprtps_path_set_passive (MPRTPSPath * this);
gboolean mprtps_path_is_non_lossy (MPRTPSPath * this);
void mprtps_path_set_lossy (MPRTPSPath * this);
void mprtps_path_set_non_lossy (MPRTPSPath * this);
gboolean mprtps_path_is_non_congested (MPRTPSPath * this);
void mprtps_path_set_congested (MPRTPSPath * this);
void mprtps_path_set_non_congested (MPRTPSPath * this);
guint8 mprtps_path_get_id (MPRTPSPath * this);
guint32 mprtps_path_get_sent_packet_num (MPRTPSPath * this);
guint32 mprtps_path_get_total_sent_packet_num (MPRTPSPath * this);
void mprtps_path_process_rtp_packet (MPRTPSPath * this, guint ext_header_id, GstRTPBuffer * rtp);
guint32 mprtps_path_get_sent_payload_bytes (MPRTPSPath * this);
guint32 mprtps_path_get_total_sent_payload_bytes (MPRTPSPath * this);
MPRTPSPathState mprtps_path_get_state (MPRTPSPath * this);
void mprtps_path_set_state (MPRTPSPath * this, MPRTPSPathState state);
GstClockTime mprtps_path_get_time_sent_to_passive(MPRTPSPath *this);
G_END_DECLS
#endif /* MPRTPSPATH_H_ */
