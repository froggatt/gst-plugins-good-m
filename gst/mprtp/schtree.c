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
#include "schtree.h"
#include <math.h>


GST_DEBUG_CATEGORY_STATIC (schtree_debug_category);
#define GST_CAT_DEFAULT schtree_debug_category

G_DEFINE_TYPE (SchTree, schtree, G_TYPE_OBJECT);

//----------------------------------------------------------------------
//-------- Private functions belongs to Scheduler tree object ----------
//----------------------------------------------------------------------
static SchNode *_schnode_ctor (void);
static void schtree_finalize (GObject * object);
static MPRTPSSubflow *schtree_get_actual (SchTree * tree);
static MPRTPSSubflow *schtree_get_next (SchTree * tree);
//static void _schtree_delete(SchNode* parent, SchNode* node,
//      MPRTPSSubflow* searched_value, gint *change, gint level_value);
static void _schtree_insert (SchNode ** node,
    MPRTPSSubflow * path, gint * change, gint level_value);
static void schtree_commit_changes (SchTree * tree);
static gboolean schtree_has_nc_nl_path (SchTree * this);
static void _schnode_overlap_trees (SchNode * old_node, SchNode * new_node);
static void schtree_setup_change (SchTree * tree, MPRTPSSubflow * path,
    gfloat value);
static void schtree_print (SchTree * tree);
static void schtree_delete_path (SchTree * tree, MPRTPSSubflow * path);
static void schtree_setup_bid_values (SchTree * tree,
    gfloat non_congested_non_lossy_share);
static void schtree_change_bid_values (SchTree * tree,
    gfloat non_congested_non_lossy_share_change);
static void _schnode_rdtor (SchNode * node);
static void _print_tree (SchNode * node, gint top, gint level);
//----------------------------------------------------------------------
//--------- Private functions implementations to SchTree object --------
//----------------------------------------------------------------------

void
schtree_class_init (SchTreeClass * klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->finalize = schtree_finalize;

  GST_DEBUG_CATEGORY_INIT (schtree_debug_category, "schtree", 0,
      "Scheduling Tree");
}

void
schtree_finalize (GObject * object)
{

}

void
schtree_init (SchTree * tree)
{
  gint i;
  tree->setup_sending_rate = schtree_setup_change;
  tree->delete_path = schtree_delete_path;
  tree->commit_changes = schtree_commit_changes;
  tree->get_actual = schtree_get_actual;
  tree->setup_bid_values = schtree_setup_bid_values;
  tree->change_bid_values = schtree_change_bid_values;
  tree->has_nc_nl_path = schtree_has_nc_nl_path;

  tree->nc_nl_path_exists = FALSE;
  //tree->get_max_value = schtree_get_max_value;
  tree->get_next = schtree_get_next;
  tree->print = schtree_print;
  tree->max_value = 128;
  tree->root = NULL;
  for (i = 0; i < MPRTP_SENDER_SCHTREE_MAX_PATH_NUM; ++i) {
    tree->paths[i] = NULL;
    tree->path_values[i] = 0;
  }
  tree->bid_const = 100.0;
}

static void
_schtree_loadup (SchNode * node, MPRTPSSubflow * path)
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
schtree_setup_bid_values (SchTree * tree, gfloat non_congested_non_lossy_share)
{
  g_rw_lock_writer_lock (&tree->rwmutex);
  tree->non_congested_bid = non_congested_non_lossy_share * tree->bid_const;
  tree->lossy_bid =
      (1.0 - non_congested_non_lossy_share) * 0.75 * tree->bid_const;
  tree->congested_bid =
      (1.0 - non_congested_non_lossy_share) * 0.25 * tree->bid_const;
//  g_print("bid values: %f-%f-%f\n",tree->non_congested_bid,
//                tree->lossy_bid, tree->congested_bid);

  g_rw_lock_writer_unlock (&tree->rwmutex);
}

void
schtree_change_bid_values (SchTree * tree,
    gfloat non_congested_non_lossy_share_change)
{
  gfloat new_non_congested_non_lossy_share;
  g_rw_lock_reader_lock (&tree->rwmutex);
  new_non_congested_non_lossy_share =
      tree->non_congested_bid / tree->bid_const * (1.0 +
      non_congested_non_lossy_share_change);
  g_rw_lock_reader_lock (&tree->rwmutex);
  schtree_setup_bid_values (tree, new_non_congested_non_lossy_share);
}

gboolean
schtree_has_nc_nl_path (SchTree * this)
{
  gboolean result;
  g_rw_lock_reader_lock (&this->rwmutex);
  result = this->nc_nl_path_exists;
  g_rw_lock_reader_unlock (&this->rwmutex);
  return result;
}

void
schtree_commit_changes (SchTree * tree)
{
  gint32 index, max_index = 0, path_num = 0;
  gfloat sum = 0.0;
  gfloat max_value = 0.0, actual_value;
  SchNode *new_root = NULL;
  MPRTPSSubflow *path;
  gboolean nc_nl_bid_added = FALSE;
  gboolean l_bid_added = FALSE;
  gboolean c_bid_added = FALSE;
  gfloat nc_nl_sum = 0.0, c_sum = 0.0, l_sum = 0.0, bid_sum = 0.0;
  gboolean non_congested, non_lossy;

  g_rw_lock_writer_lock (&tree->rwmutex);
  tree->nc_nl_path_exists = FALSE;

  for (index = 0; index < MPRTP_SENDER_SCHTREE_MAX_PATH_NUM &&
      tree->paths[index] != NULL; ++index, ++path_num) {
    path = tree->paths[index];
    actual_value = (tree->path_values[index] + tree->path_delta_values[index]);
    tree->path_delta_values[index] = 0.0;
    if (actual_value <= 0.0) {
      actual_value = 0.0;
    }
    //g_print("tree->path_values[index]: %f, tree->path_delta_values[index]: %f, "
    //      "actual value: %f\n", tree->path_values[index], tree->path_delta_values[index], actual_value);
    tree->path_values[index] = actual_value;

    if (max_value < actual_value) {
      max_index = index;
    }
    if (path->is_active (path)) {
      non_congested = path->is_non_congested (path);
      non_lossy = path->is_non_lossy (path);
      if (non_congested && non_lossy) {
        nc_nl_sum += actual_value;
        bid_sum += (!nc_nl_bid_added) ? tree->non_congested_bid : 0.0;
        nc_nl_bid_added = TRUE;
        tree->nc_nl_path_exists = TRUE;
      } else if (non_congested) {
        l_sum += actual_value;
        bid_sum += (!l_bid_added) ? tree->lossy_bid : 0.0;
        l_bid_added = TRUE;
      } else {
        c_sum += actual_value;
        bid_sum += (!c_bid_added) ? tree->congested_bid : 0.0;
        c_bid_added = TRUE;
      }
      sum += actual_value;
    }
  }

  for (index = 0; index < path_num; ++index) {
    gint insert_value;
    path = tree->paths[index];
    non_congested = path->is_non_congested (path);
    non_lossy = path->is_non_lossy (path);
    if (non_congested && non_lossy) {
      actual_value = tree->path_values[index] / nc_nl_sum *
          tree->non_congested_bid / bid_sum * (gfloat) tree->max_value;
    } else if (non_congested) {
      actual_value = tree->path_values[index] / l_sum *
          tree->lossy_bid / bid_sum * (gfloat) tree->max_value;
    } else {
      actual_value = tree->path_values[index] / c_sum *
          tree->congested_bid / bid_sum * (gfloat) tree->max_value;
    }
//
//      actual_value =  tree->path_values[index] /
//                                  sum * (gfloat) tree->max_value;

    insert_value = (guint16) roundf (actual_value);

    //g_print("subflow %d new rate: %f\n", path->get_id(path),
    //      tree->path_values[index] / sum);
//
    //g_print ("%p -> insert subflow %d: %d ( %f%%)\n",
    tree, path->get_id (path), insert_value,
        actual_value / tree->max_value * 100.0);
    //g_print("path value is %f\n",tree->path_values[index]);
    if (tree->path_values[index] > 0.0) {
      _schtree_insert (&new_root, path, &insert_value, tree->max_value);
    }
  }

  //check 128 integrity
  _schnode_rdtor (tree->root);
  _schnode_overlap_trees (tree->root, new_root);
  tree->root = new_root;
  _schtree_loadup (tree->root, tree->paths[max_index]);
  //tree->print(tree);
  g_rw_lock_writer_unlock (&tree->rwmutex);

}


void _schnode_rdtor (SchNode * node)
{
  if (node == NULL) {
    return;
  }
  _schnode_rdtor (node->left);
  _schnode_rdtor (node->right);
  g_free (node);
}

void _schnode_overlap_trees (SchNode * old_node, SchNode * new_node)
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

void schtree_setup_change (SchTree * tree, MPRTPSSubflow * path, gfloat value)
{
  gint32 index;
  g_rw_lock_writer_lock (&tree->rwmutex);
  for (index = 0;
      index < MPRTP_SENDER_SCHTREE_MAX_PATH_NUM && tree->paths[index] != path;
      ++index);

  if (index < MPRTP_SENDER_SCHTREE_MAX_PATH_NUM) {
    tree->path_delta_values[index] = value - tree->path_values[index];
    goto schtree_set_path_and_values_done;
  }
  for (index = 0; tree->paths[index] != NULL &&
      index < MPRTP_SENDER_SCHTREE_MAX_PATH_NUM; ++index);

  if (index == MPRTP_SENDER_SCHTREE_MAX_PATH_NUM) {
    goto schtree_set_path_and_values_done;
  }

  tree->paths[index] = path;
  tree->path_values[index] = 0.0;
  tree->path_delta_values[index] = value;

schtree_set_path_and_values_done:
  //g_print("schtree_setup_change: subflow: %d, path: %p, desired value:%f, actual value: %f, delta value: %f\n",
  //        path->get_id(path), path, value, tree->path_values[index], tree->path_delta_values[index]);
  g_rw_lock_writer_unlock (&tree->rwmutex);
}


void schtree_delete_path (SchTree * tree, MPRTPSSubflow * path)
{
  gint32 index;
  g_rw_lock_writer_lock (&tree->rwmutex);
  for (index = 0;
      index < MPRTP_SENDER_SCHTREE_MAX_PATH_NUM && tree->paths[index] != path;
      ++index);

  if (index == MPRTP_SENDER_SCHTREE_MAX_PATH_NUM) {
    goto schtree_delete_path_done;
  }

  tree->paths[index] = NULL;
  tree->path_values[index] = 0.0;
  tree->path_delta_values[index] = 0.0;

schtree_delete_path_done:
  //g_print("schtree_setup_change: subflow: %d, desired value:%f, actual value: %f, delta value: %f\n",
  //        path->get_id(path), value, tree->path_values[index], tree->path_delta_values[index]);
  g_rw_lock_writer_unlock (&tree->rwmutex);
}

void _schtree_insert (SchNode ** node, MPRTPSSubflow * path, gint * change,
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

//
//void
//_schtree_delete(SchNode* parent, SchNode* node,
//      MPRTPSSubflow* searched_value, gint *change, gint level_value)
//{
//  if(node == NULL || *change < 1){
//    return;
//  }
//  if(node->path != NULL){
//    if(node->path != searched_value){
//      return;
//    }
//    g_free(node);
//    *change -= level_value;
//    if(parent == NULL){
//      return;
//    }
//    if(parent->left == node){
//      parent->left = NULL;
//    }else{
//      parent->right = NULL;
//    }
//    return;
//  }
//  if(node->left != NULL && node->left->path != NULL){
//      _schtree_delete(node, node->right, searched_value, change, level_value>>1);
//      _schtree_delete(node, node->left, searched_value, change, level_value>>1);
//  }else if(node->right != NULL){
//      _schtree_delete(node, node->left, searched_value, change, level_value>>1);
//      _schtree_delete(node, node->right, searched_value, change, level_value>>1);
//  }
//  if(node->left != NULL || node->right != NULL || parent == NULL){
//    return;
//  }
//  g_free(node);
//  if(parent->left == node){
//    parent->left = NULL;
//  }else{
//    parent->right = NULL;
//  }
//}

SchNode *_schnode_ctor (void)
{
  SchNode *result = (SchNode *) g_malloc0 (sizeof (SchNode));
  result->left = NULL;
  result->right = NULL;
  result->next = NULL;
  result->path = NULL;
  return result;
}

MPRTPSSubflow *schtree_get_next (SchTree * tree)
{
  MPRTPSSubflow *result;
  SchNode *selected;
  g_rw_lock_writer_lock (&tree->rwmutex);
  selected = tree->root;
  while (selected->path == NULL) {
    selected->next =
        (selected->next == selected->left) ? selected->right : selected->left;
    selected = selected->next;
  }
  result = selected->path;
  g_rw_lock_writer_unlock (&tree->rwmutex);
  return result;
}

MPRTPSSubflow *schtree_get_actual (SchTree * tree)
{
  MPRTPSSubflow *result;
  SchNode *selected;
  g_rw_lock_writer_lock (&tree->rwmutex);
  selected = tree->root;
  while (selected->path == NULL) {
    selected->next = (selected->next == NULL) ? selected->left : selected->next;
    selected = selected->next;
    //g_print("node: %p l->%p r->%p p: %p\n",
    //        selected, selected->left, selected->right, selected->path);
  }
  result = selected->path;
  //g_print("selected: %p->path:%p\n", selected, selected->path);
  g_rw_lock_writer_unlock (&tree->rwmutex);
  return result;
}

//guint16
//schtree_get_max_value(SchTree* tree)
//{
//  guint16 result;
//  g_rw_lock_reader_lock (&tree->rwmutex);
//  result = tree->max_value;
//  g_rw_lock_reader_unlock (&tree->rwmutex);
//  return result;
//}

void schtree_print (SchTree * tree)
{
  _print_tree (tree->root, 128, 0);
}

void _print_tree (SchNode * node, gint top, gint level)
{
  gint i;
  if (node == NULL) {
    return;
  }
  for (i = 0; i < level; ++i)
    g_print ("--");
  //g_print("%p (%d)\n", node->path, top>>level);
  _print_tree (node->left, top, level + 1);
  _print_tree (node->right, top, level + 1);
}
