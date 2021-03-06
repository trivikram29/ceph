// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "librbd/DiffIterate.h"
#include "librbd/ImageCtx.h"
#include "librbd/internal.h"
#include "include/rados/librados.hpp"
#include "include/interval_set.h"
#include "common/errno.h"
#include "common/Mutex.h"
#include "common/Throttle.h"
#include "librados/snap_set_diff.h"
#include <boost/tuple/tuple.hpp>
#include <list>
#include <map>
#include <vector>

#define dout_subsys ceph_subsys_rbd
#undef dout_prefix
#define dout_prefix *_dout << "librbd::DiffIterate: "

namespace librbd {

namespace {

enum ObjectDiffState {
  OBJECT_DIFF_STATE_NONE    = 0,
  OBJECT_DIFF_STATE_UPDATED = 1,
  OBJECT_DIFF_STATE_HOLE    = 2
};

class DiffContext {
public:
  typedef boost::tuple<uint64_t, size_t, bool> Diff;
  typedef std::list<Diff> Diffs;

  bool whole_object;
  uint64_t from_snap_id;
  uint64_t end_snap_id;
  interval_set<uint64_t> parent_diff;

  DiffContext(ImageCtx &image_ctx, DiffIterate::Callback callback,
              void *callback_arg, bool _whole_object, uint64_t _from_snap_id,
              uint64_t _end_snap_id)
    : whole_object(_whole_object), from_snap_id(_from_snap_id),
      end_snap_id(_end_snap_id), m_lock("librbd::DiffContext::m_lock"),
      m_image_ctx(image_ctx), m_callback(callback),
      m_callback_arg(callback_arg), m_pending_ops(0), m_return_value(0),
      m_next_request(0), m_waiting_request(0)
  {
  }

  int invoke_callback() {
    Mutex::Locker locker(m_lock);
    if (m_return_value < 0) {
      return m_return_value;
    }

    std::map<uint64_t, Diffs>::iterator it;
    while ((it = m_request_diffs.begin()) != m_request_diffs.end() &&
           it->first == m_waiting_request) {
      Diffs diffs = it->second;
      m_request_diffs.erase(it);

      for (Diffs::const_iterator d = diffs.begin(); d != diffs.end(); ++d) {
        m_lock.Unlock();
        int r = m_callback(d->get<0>(), d->get<1>(), d->get<2>(),
                           m_callback_arg);
        m_lock.Lock();

        if (m_return_value == 0 && r < 0) {
          m_return_value = r;
          return m_return_value;
        }
      }
      ++m_waiting_request;
    }
    return 0;
  }

  int wait_for_ret() {
    Mutex::Locker locker(m_lock);
    while (m_pending_ops > 0) {
      m_cond.Wait(m_lock);
    }
    return m_return_value;
  }

  uint64_t start_op() {
    Mutex::Locker locker(m_lock);
    while (m_pending_ops >= m_image_ctx.concurrent_management_ops) {
        m_cond.Wait(m_lock);
    }
    ++m_pending_ops;
    return m_next_request++;
  }

  void finish_op(uint64_t request_num, int r, const Diffs &diffs) {
    Mutex::Locker locker(m_lock);
    m_request_diffs[request_num] = diffs;

    if (m_return_value == 0 && r < 0) {
      m_return_value = r;
    }

    --m_pending_ops;
    m_cond.Signal();
  }

private:
  Mutex m_lock;
  Cond m_cond;

  ImageCtx &m_image_ctx;
  DiffIterate::Callback m_callback;
  void *m_callback_arg;

  uint32_t m_pending_ops;
  int m_return_value;

  uint64_t m_next_request;
  uint64_t m_waiting_request;

  std::map<uint64_t, Diffs> m_request_diffs;
};

class C_DiffObject : public Context {
public:
  C_DiffObject(ImageCtx &image_ctx, librados::IoCtx &head_ctx,
               DiffContext &diff_context, const std::string &oid,
               uint64_t offset, const std::vector<ObjectExtent> &object_extents)
    : m_image_ctx(image_ctx), m_head_ctx(head_ctx),
      m_diff_context(diff_context), m_oid(oid), m_offset(offset),
      m_object_extents(object_extents), m_snap_ret(0)
  {
    m_request_num = m_diff_context.start_op();
  }

  void send() {
    librados::ObjectReadOperation op;
    op.list_snaps(&m_snap_set, &m_snap_ret);

    librados::AioCompletion *rados_completion =
      librados::Rados::aio_create_completion(this, NULL, rados_ctx_cb);
    int r = m_head_ctx.aio_operate(m_oid, rados_completion, &op, NULL);
    assert(r == 0);
    rados_completion->release();
  }

protected:
  virtual void finish(int r) {
    CephContext *cct = m_image_ctx.cct;
    if (r == 0 && m_snap_ret < 0) {
      r = m_snap_ret;
    }

    DiffContext::Diffs diffs;
    if (r == 0) {
      ldout(cct, 20) << "object " << m_oid << ": list_snaps complete" << dendl;
      compute_diffs(&diffs);
    } else if (r == -ENOENT) {
      ldout(cct, 20) << "object " << m_oid << ": list_snaps (not found)"
                     << dendl;
      r = 0;
      compute_parent_overlap(&diffs);
    } else {
      ldout(cct, 20) << "object " << m_oid << ": list_snaps failed: "
                     << cpp_strerror(r) << dendl;
    }

    m_diff_context.finish_op(m_request_num, r, diffs);
  }

private:
  ImageCtx &m_image_ctx;
  librados::IoCtx &m_head_ctx;
  DiffContext &m_diff_context;
  uint64_t m_request_num;
  std::string m_oid;
  uint64_t m_offset;
  std::vector<ObjectExtent> m_object_extents;

  librados::snap_set_t m_snap_set;
  int m_snap_ret;

  void compute_diffs(DiffContext::Diffs *diffs) {
    CephContext *cct = m_image_ctx.cct;

    // calc diff from from_snap_id -> to_snap_id
    interval_set<uint64_t> diff;
    bool end_exists;
    calc_snap_set_diff(cct, m_snap_set, m_diff_context.from_snap_id,
                       m_diff_context.end_snap_id, &diff, &end_exists);
    ldout(cct, 20) << "  diff " << diff << " end_exists=" << end_exists
                   << dendl;
    if (diff.empty()) {
      return;
    } else if (m_diff_context.whole_object) {
      // provide the full object extents to the callback
      for (vector<ObjectExtent>::iterator q = m_object_extents.begin();
           q != m_object_extents.end(); ++q) {
        diffs->push_back(boost::make_tuple(m_offset + q->offset, q->length,
                                           end_exists));
      }
      return;
    }

    for (vector<ObjectExtent>::iterator q = m_object_extents.begin();
         q != m_object_extents.end(); ++q) {
      ldout(cct, 20) << "diff_iterate object " << m_oid << " extent "
                     << q->offset << "~" << q->length << " from "
                     << q->buffer_extents << dendl;
      uint64_t opos = q->offset;
      for (vector<pair<uint64_t,uint64_t> >::iterator r =
             q->buffer_extents.begin();
           r != q->buffer_extents.end(); ++r) {
        interval_set<uint64_t> overlap;  // object extents
        overlap.insert(opos, r->second);
        overlap.intersection_of(diff);
        ldout(m_image_ctx.cct, 20) << " opos " << opos
    			             << " buf " << r->first << "~" << r->second
    			             << " overlap " << overlap << dendl;
        for (interval_set<uint64_t>::iterator s = overlap.begin();
    	       s != overlap.end(); ++s) {
          uint64_t su_off = s.get_start() - opos;
          uint64_t logical_off = m_offset + r->first + su_off;
          ldout(cct, 20) << "   overlap extent " << s.get_start() << "~"
                         << s.get_len() << " logical " << logical_off << "~"
                         << s.get_len() << dendl;
          diffs->push_back(boost::make_tuple(logical_off, s.get_len(),
                           end_exists));
        }
        opos += r->second;
      }
      assert(opos == q->offset + q->length);
    }
  }

  void compute_parent_overlap(DiffContext::Diffs *diffs) {
    if (m_diff_context.from_snap_id == 0 &&
        !m_diff_context.parent_diff.empty()) {
      // report parent diff instead
      for (vector<ObjectExtent>::iterator q = m_object_extents.begin();
           q != m_object_extents.end(); ++q) {
        for (vector<pair<uint64_t,uint64_t> >::iterator r =
               q->buffer_extents.begin();
             r != q->buffer_extents.end(); ++r) {
          interval_set<uint64_t> o;
          o.insert(m_offset + r->first, r->second);
          o.intersection_of(m_diff_context.parent_diff);
          ldout(m_image_ctx.cct, 20) << " reporting parent overlap " << o
                                     << dendl;
          for (interval_set<uint64_t>::iterator s = o.begin(); s != o.end();
               ++s) {
            diffs->push_back(boost::make_tuple(s.get_start(), s.get_len(),
                             true));
          }
        }
      }
    }
  }
};

} // anonymous namespace

int DiffIterate::execute() {
  CephContext* cct = m_image_ctx.cct;

  librados::IoCtx head_ctx;
  librados::snap_t from_snap_id = 0;
  librados::snap_t end_snap_id;
  uint64_t from_size = 0;
  uint64_t end_size;
  {
    RWLock::RLocker md_locker(m_image_ctx.md_lock);
    RWLock::RLocker snap_locker(m_image_ctx.snap_lock);
    head_ctx.dup(m_image_ctx.data_ctx);
    if (m_from_snap_name) {
      from_snap_id = m_image_ctx.get_snap_id(m_from_snap_name);
      from_size = m_image_ctx.get_image_size(from_snap_id);
    }
    end_snap_id = m_image_ctx.snap_id;
    end_size = m_image_ctx.get_image_size(end_snap_id);
  }

  if (from_snap_id == CEPH_NOSNAP) {
    return -ENOENT;
  }
  if (from_snap_id == end_snap_id) {
    // no diff.
    return 0;
  }
  if (from_snap_id >= end_snap_id) {
    return -EINVAL;
  }

  int r;
  bool fast_diff_enabled = false;
  BitVector<2> object_diff_state;
  {
    RWLock::RLocker snap_locker(m_image_ctx.snap_lock);
    if (m_whole_object && (m_image_ctx.features & RBD_FEATURE_FAST_DIFF) != 0) {
      r = diff_object_map(from_snap_id, end_snap_id, &object_diff_state);
      if (r < 0) {
        ldout(cct, 5) << "fast diff disabled" << dendl;
      } else {
        ldout(cct, 5) << "fast diff enabled" << dendl;
        fast_diff_enabled = true;
      }
    }
  }

  // we must list snaps via the head, not end snap
  head_ctx.snap_set_read(CEPH_SNAPDIR);

  ldout(cct, 5) << "diff_iterate from " << from_snap_id << " to "
                << end_snap_id << " size from " << from_size
                << " to " << end_size << dendl;

  // check parent overlap only if we are comparing to the beginning of time
  DiffContext diff_context(m_image_ctx, m_callback, m_callback_arg,
                           m_whole_object, from_snap_id, end_snap_id);
  if (m_include_parent && from_snap_id == 0) {
    RWLock::RLocker l(m_image_ctx.snap_lock);
    RWLock::RLocker l2(m_image_ctx.parent_lock);
    uint64_t overlap = end_size;
    m_image_ctx.get_parent_overlap(from_snap_id, &overlap);
    r = 0;
    if (m_image_ctx.parent && overlap > 0) {
      ldout(cct, 10) << " first getting parent diff" << dendl;
      DiffIterate diff_parent(*m_image_ctx.parent, NULL, 0, overlap,
                              m_include_parent, m_whole_object,
                              &DiffIterate::simple_diff_cb,
                              &diff_context.parent_diff);
      r = diff_parent.execute();
    }
    if (r < 0) {
      return r;
    }
  }

  uint64_t period = m_image_ctx.get_stripe_period();
  uint64_t off = m_offset;
  uint64_t left = m_length;

  while (left > 0) {
    uint64_t period_off = off - (off % period);
    uint64_t read_len = min(period_off + period - off, left);

    // map to extents
    map<object_t,vector<ObjectExtent> > object_extents;
    Striper::file_to_extents(cct, m_image_ctx.format_string,
                             &m_image_ctx.layout, off, read_len, 0,
                             object_extents, 0);

    // get snap info for each object
    for (map<object_t,vector<ObjectExtent> >::iterator p =
           object_extents.begin();
         p != object_extents.end(); ++p) {
      ldout(cct, 20) << "object " << p->first << dendl;

      if (fast_diff_enabled) {
        const uint64_t object_no = p->second.front().objectno;
        if (object_diff_state[object_no] != OBJECT_DIFF_STATE_NONE) {
          bool updated = (object_diff_state[object_no] ==
                            OBJECT_DIFF_STATE_UPDATED);
          for (std::vector<ObjectExtent>::iterator q = p->second.begin();
               q != p->second.end(); ++q) {
            r = m_callback(off + q->offset, q->length, updated, m_callback_arg);
            if (r < 0) {
              return r;
            }
          }
        }
      } else {
        C_DiffObject *diff_object = new C_DiffObject(m_image_ctx, head_ctx,
                                                     diff_context,
                                                     p->first.name, off,
                                                     p->second);
        diff_object->send();

        r = diff_context.invoke_callback();
        if (r < 0) {
          diff_context.wait_for_ret();
          return r;
        }
      }
    }

    left -= read_len;
    off += read_len;
  }

  r = diff_context.wait_for_ret();
  if (r < 0) {
    return r;
  }

  r = diff_context.invoke_callback();
  return r;
}

int DiffIterate::diff_object_map(uint64_t from_snap_id, uint64_t to_snap_id,
                                 BitVector<2>* object_diff_state) {
  assert(m_image_ctx.snap_lock.is_locked());
  CephContext* cct = m_image_ctx.cct;

  bool diff_from_start = (from_snap_id == 0);
  if (from_snap_id == 0) {
    if (!m_image_ctx.snaps.empty()) {
      from_snap_id = m_image_ctx.snaps.back();
    } else {
      from_snap_id = CEPH_NOSNAP;
    }
  }

  object_diff_state->clear();
  int r;
  uint64_t current_snap_id = from_snap_id;
  uint64_t next_snap_id = to_snap_id;
  BitVector<2> prev_object_map;
  bool prev_object_map_valid = false;
  while (true) {
    uint64_t current_size = m_image_ctx.size;
    if (current_snap_id != CEPH_NOSNAP) {
      std::map<librados::snap_t, SnapInfo>::const_iterator snap_it =
        m_image_ctx.snap_info.find(current_snap_id);
      assert(snap_it != m_image_ctx.snap_info.end());
      current_size = snap_it->second.size;

      ++snap_it;
      if (snap_it != m_image_ctx.snap_info.end()) {
        next_snap_id = snap_it->first;
      } else {
        next_snap_id = CEPH_NOSNAP;
      }
    }

    uint64_t flags;
    r = m_image_ctx.get_flags(from_snap_id, &flags);
    if (r < 0) {
      lderr(cct) << "diff_object_map: failed to retrieve image flags" << dendl;
      return r;
    }
    if ((flags & RBD_FLAG_FAST_DIFF_INVALID) != 0) {
      ldout(cct, 1) << "diff_object_map: cannot perform fast diff on invalid "
                    << "object map" << dendl;
      return -EINVAL;
    }

    BitVector<2> object_map;
    std::string oid(ObjectMap::object_map_name(m_image_ctx.id,
                                               current_snap_id));
    r = cls_client::object_map_load(&m_image_ctx.md_ctx, oid, &object_map);
    if (r < 0) {
      lderr(cct) << "diff_object_map: failed to load object map " << oid
                 << dendl;
      return r;
    }
    ldout(cct, 20) << "diff_object_map: loaded object map " << oid << dendl;

    uint64_t num_objs = Striper::get_num_objects(m_image_ctx.layout,
                                                 current_size);
    if (object_map.size() < num_objs) {
      ldout(cct, 1) << "diff_object_map: object map too small: "
                    << object_map.size() << " < " << num_objs << dendl;
      return -EINVAL;
    }
    object_map.resize(num_objs);

    uint64_t overlap = MIN(object_map.size(), prev_object_map.size());
    for (uint64_t i = 0; i < overlap; ++i) {
      ldout(cct, 20) << __func__ << ": object state: " << i << " "
                     << static_cast<uint32_t>(prev_object_map[i])
                     << "->" << static_cast<uint32_t>(object_map[i]) << dendl;
      if (object_map[i] == OBJECT_NONEXISTENT) {
        if (prev_object_map[i] != OBJECT_NONEXISTENT) {
          (*object_diff_state)[i] = OBJECT_DIFF_STATE_HOLE;
        }
      } else if (object_map[i] == OBJECT_EXISTS ||
                 (prev_object_map[i] != object_map[i] &&
                  !(prev_object_map[i] == OBJECT_EXISTS &&
                    object_map[i] == OBJECT_EXISTS_CLEAN))) {
        (*object_diff_state)[i] = OBJECT_DIFF_STATE_UPDATED;
      }
    }
    ldout(cct, 20) << "diff_object_map: computed overlap diffs" << dendl;

    object_diff_state->resize(object_map.size());
    if (object_map.size() > prev_object_map.size() &&
        (diff_from_start || prev_object_map_valid)) {
      for (uint64_t i = overlap; i < object_diff_state->size(); ++i) {
        ldout(cct, 20) << __func__ << ": object state: " << i << " "
                       << "->" << static_cast<uint32_t>(object_map[i]) << dendl;
        if (object_map[i] == OBJECT_NONEXISTENT) {
          (*object_diff_state)[i] = OBJECT_DIFF_STATE_NONE;
        } else {
          (*object_diff_state)[i] = OBJECT_DIFF_STATE_UPDATED;
        }
      }
    }
    ldout(cct, 20) << "diff_object_map: computed resize diffs" << dendl;

    if (current_snap_id == next_snap_id || next_snap_id > to_snap_id) {
      break;
    }
    current_snap_id = next_snap_id;
    prev_object_map = object_map;
    prev_object_map_valid = true;
  }
  return 0;
}

int DiffIterate::simple_diff_cb(uint64_t off, size_t len, int exists,
                                void *arg) {
  // This reads the existing extents in a parent from the beginning
  // of time.  Since images are thin-provisioned, the extents will
  // always represent data, not holes.
  assert(exists);
  interval_set<uint64_t> *diff = static_cast<interval_set<uint64_t> *>(arg);
  diff->insert(off, len);
  return 0;
}

} // namespace librbd
