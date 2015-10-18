/*
 * smanctrler.h
 *
 *  Created on: Jun 30, 2015
 *      Author: balazs
 */

#ifndef STREAM_JOINER_H_
#define STREAM_JOINER_H_

#include <gst/gst.h>

#include "mprtprpath.h"

typedef struct _StreamJoiner StreamJoiner;
typedef struct _StreamJoinerClass StreamJoinerClass;
typedef struct _Heap Heap;

#define STREAM_JOINER_TYPE             (stream_joiner_get_type())
#define STREAM_JOINER(src)             (G_TYPE_CHECK_INSTANCE_CAST((src),STREAM_JOINER_TYPE,StreamJoiner))
#define STREAM_JOINER_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST((klass),STREAM_JOINER_TYPE,StreamJoinerClass))
#define STREAM_JOINER_IS_SOURCE(src)          (G_TYPE_CHECK_INSTANCE_TYPE((src),STREAM_JOINER_TYPE))
#define STREAM_JOINER_IS_SOURCE_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE((klass),STREAM_JOINER_TYPE))
#define STREAM_JOINER_CAST(src)        ((StreamJoiner *)(src))

#define MPRTP_SENDER_SCHTREE_MAX_PATH_NUM 32


struct _StreamJoiner
{
  GObject          object;

  GstTask*          thread;
  GRecMutex         thread_mutex;
  GList*            queued;
  GHashTable*       subflows;
  GRWLock           rwmutex;

  GstClockTime      playout_delay;
  Heap*             packets_heap;
  GQueue*           heap_items_pool;

  gint              subflow_num;
  GstClock*         sysclock;
  void            (*send_mprtp_packet_func)(gpointer,GstBuffer*);
  gpointer          send_mprtp_packet_data;
};

struct _StreamJoinerClass{
  GObjectClass parent_class;
};

//Class functions
void stream_joiner_set_sending(StreamJoiner* this, gpointer data, void (*func)(gpointer,GstBuffer*));

void
stream_joiner_add_path(StreamJoiner * this, guint8 subflow_id, MpRTPRPath *path);

void
stream_joiner_rem_path(StreamJoiner * this, guint8 subflow_id);

void stream_joiner_set_playout_delay(StreamJoiner *this, GstClockTime delay);

GType stream_joiner_get_type (void);

#endif /* STREAM_JOINER_H_ */
