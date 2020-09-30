// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_LIBRBD_IO_TYPES_H
#define CEPH_LIBRBD_IO_TYPES_H

#include "include/int_types.h"
#include "include/rados/rados_types.hpp"
#include "common/interval_map.h"
#include "osdc/StriperTypes.h"
#include <iosfwd>
#include <map>
#include <vector>

struct Context;

namespace librbd {
namespace io {

typedef enum {
  AIO_TYPE_NONE = 0,
  AIO_TYPE_GENERIC,
  AIO_TYPE_OPEN,
  AIO_TYPE_CLOSE,
  AIO_TYPE_READ,
  AIO_TYPE_WRITE,
  AIO_TYPE_DISCARD,
  AIO_TYPE_FLUSH,
  AIO_TYPE_WRITESAME,
  AIO_TYPE_COMPARE_AND_WRITE,
} aio_type_t;

enum FlushSource {
  FLUSH_SOURCE_USER,
  FLUSH_SOURCE_INTERNAL,
  FLUSH_SOURCE_SHUTDOWN,
  FLUSH_SOURCE_EXCLUSIVE_LOCK,
  FLUSH_SOURCE_EXCLUSIVE_LOCK_SKIP_REFRESH,
  FLUSH_SOURCE_REFRESH,
  FLUSH_SOURCE_WRITEBACK,
  FLUSH_SOURCE_WRITE_BLOCK,
};

enum Direction {
  DIRECTION_READ,
  DIRECTION_WRITE,
  DIRECTION_BOTH
};

enum DispatchResult {
  DISPATCH_RESULT_INVALID,
  DISPATCH_RESULT_RESTART,
  DISPATCH_RESULT_CONTINUE,
  DISPATCH_RESULT_COMPLETE
};

enum ImageDispatchLayer {
  IMAGE_DISPATCH_LAYER_NONE = 0,
  IMAGE_DISPATCH_LAYER_API_START = IMAGE_DISPATCH_LAYER_NONE,
  IMAGE_DISPATCH_LAYER_QUEUE,
  IMAGE_DISPATCH_LAYER_QOS,
  IMAGE_DISPATCH_LAYER_EXCLUSIVE_LOCK,
  IMAGE_DISPATCH_LAYER_REFRESH,
  IMAGE_DISPATCH_LAYER_INTERNAL_START = IMAGE_DISPATCH_LAYER_REFRESH,
  IMAGE_DISPATCH_LAYER_JOURNAL,
  IMAGE_DISPATCH_LAYER_WRITE_BLOCK,
  IMAGE_DISPATCH_LAYER_WRITEBACK_CACHE,
  IMAGE_DISPATCH_LAYER_CORE,
  IMAGE_DISPATCH_LAYER_LAST
};

enum {
  IMAGE_DISPATCH_FLAG_QOS_IOPS_THROTTLE       = 1 << 0,
  IMAGE_DISPATCH_FLAG_QOS_BPS_THROTTLE        = 1 << 1,
  IMAGE_DISPATCH_FLAG_QOS_READ_IOPS_THROTTLE  = 1 << 2,
  IMAGE_DISPATCH_FLAG_QOS_WRITE_IOPS_THROTTLE = 1 << 3,
  IMAGE_DISPATCH_FLAG_QOS_READ_BPS_THROTTLE   = 1 << 4,
  IMAGE_DISPATCH_FLAG_QOS_WRITE_BPS_THROTTLE  = 1 << 5,
  IMAGE_DISPATCH_FLAG_QOS_BPS_MASK            = (
    IMAGE_DISPATCH_FLAG_QOS_BPS_THROTTLE |
    IMAGE_DISPATCH_FLAG_QOS_READ_BPS_THROTTLE |
    IMAGE_DISPATCH_FLAG_QOS_WRITE_BPS_THROTTLE),
  IMAGE_DISPATCH_FLAG_QOS_IOPS_MASK           = (
    IMAGE_DISPATCH_FLAG_QOS_IOPS_THROTTLE |
    IMAGE_DISPATCH_FLAG_QOS_READ_IOPS_THROTTLE |
    IMAGE_DISPATCH_FLAG_QOS_WRITE_IOPS_THROTTLE),
  IMAGE_DISPATCH_FLAG_QOS_READ_MASK           = (
    IMAGE_DISPATCH_FLAG_QOS_READ_IOPS_THROTTLE |
    IMAGE_DISPATCH_FLAG_QOS_READ_BPS_THROTTLE),
  IMAGE_DISPATCH_FLAG_QOS_WRITE_MASK          = (
    IMAGE_DISPATCH_FLAG_QOS_WRITE_IOPS_THROTTLE |
    IMAGE_DISPATCH_FLAG_QOS_WRITE_BPS_THROTTLE),
  IMAGE_DISPATCH_FLAG_QOS_MASK                = (
    IMAGE_DISPATCH_FLAG_QOS_BPS_MASK |
    IMAGE_DISPATCH_FLAG_QOS_IOPS_MASK),
};

enum ObjectDispatchLayer {
  OBJECT_DISPATCH_LAYER_NONE = 0,
  OBJECT_DISPATCH_LAYER_CACHE,
  OBJECT_DISPATCH_LAYER_CRYPTO,
  OBJECT_DISPATCH_LAYER_JOURNAL,
  OBJECT_DISPATCH_LAYER_PARENT_CACHE,
  OBJECT_DISPATCH_LAYER_SCHEDULER,
  OBJECT_DISPATCH_LAYER_CORE,
  OBJECT_DISPATCH_LAYER_LAST
};

enum {
  READ_FLAG_DISABLE_READ_FROM_PARENT            = 1UL << 0,
  READ_FLAG_DISABLE_CLIPPING                    = 1UL << 1,
};

enum {
  OBJECT_WRITE_FLAG_CREATE_EXCLUSIVE            = 1UL << 0
};

enum {
  OBJECT_DISCARD_FLAG_DISABLE_CLONE_REMOVE      = 1UL << 0,
  OBJECT_DISCARD_FLAG_DISABLE_OBJECT_MAP_UPDATE = 1UL << 1
};

enum {
  OBJECT_DISPATCH_FLAG_FLUSH                    = 1UL << 0,
  OBJECT_DISPATCH_FLAG_WILL_RETRY_ON_ERROR      = 1UL << 1
};

enum {
  LIST_SNAPS_FLAG_DISABLE_LIST_FROM_PARENT      = 1UL << 0,
  LIST_SNAPS_FLAG_WHOLE_OBJECT                  = 1UL << 1,
  LIST_SNAPS_FLAG_IGNORE_ZEROED_EXTENTS         = 1UL << 2,
};

enum SparseExtentState {
  SPARSE_EXTENT_STATE_DNE,    /* does not exist */
  SPARSE_EXTENT_STATE_ZEROED,
  SPARSE_EXTENT_STATE_DATA
};

std::ostream& operator<<(std::ostream& os, SparseExtentState state);

struct SparseExtent {
  SparseExtentState state;
  size_t length;

  SparseExtent(SparseExtentState state, size_t length)
    : state(state), length(length) {
  }

  operator SparseExtentState() const {
    return state;
  }

  bool operator==(const SparseExtent& rhs) const {
    return state == rhs.state && length == rhs.length;
  }
};

std::ostream& operator<<(std::ostream& os, const SparseExtent& state);

struct SparseExtentSplitMerge {
  SparseExtent split(uint64_t offset, uint64_t length, SparseExtent &se) const {
    return SparseExtent(se.state, se.length);
  }

  bool can_merge(const SparseExtent& left, const SparseExtent& right) const {
    return left.state == right.state;
  }

  SparseExtent merge(SparseExtent&& left, SparseExtent&& right) const {
    SparseExtent se(left);
    se.length += right.length;
    return se;
  }

  uint64_t length(const SparseExtent& se) const {
    return se.length;
  }
};

typedef interval_map<uint64_t,
                     SparseExtent,
                     SparseExtentSplitMerge> SparseExtents;

typedef std::vector<uint64_t> SnapIds;

typedef std::pair<librados::snap_t, librados::snap_t> WriteReadSnapIds;
extern const WriteReadSnapIds INITIAL_WRITE_READ_SNAP_IDS;

typedef std::map<WriteReadSnapIds, SparseExtents> SnapshotDelta;

using striper::LightweightBufferExtents;
using striper::LightweightObjectExtent;
using striper::LightweightObjectExtents;

typedef std::pair<uint64_t,uint64_t> Extent;
typedef std::vector<Extent> Extents;

struct ReadExtent {
    const uint64_t offset;
    const uint64_t length;
    const LightweightBufferExtents buffer_extents;
    ceph::bufferlist bl;
    Extents extent_map;

    ReadExtent(uint64_t offset,
               uint64_t length) : offset(offset), length(length) {};
    ReadExtent(uint64_t offset,
               uint64_t length,
               const LightweightBufferExtents&& buffer_extents)
               : offset(offset),
                 length(length),
                 buffer_extents(buffer_extents) {}
    ReadExtent(uint64_t offset,
               uint64_t length,
               const LightweightBufferExtents&& buffer_extents,
               ceph::bufferlist&& bl,
               Extents&& extent_map) : offset(offset),
                                       length(length),
                                       buffer_extents(buffer_extents),
                                       bl(bl),
                                       extent_map(extent_map) {};

    friend inline std::ostream& operator<<(
            std::ostream& os,
            const ReadExtent &extent) {
      os << "offset=" << extent.offset << ", "
         << "length=" << extent.length << ", "
         << "buffer_extents=" << extent.buffer_extents << ", "
         << "bl.length=" << extent.bl.length() << ", "
         << "extent_map=" << extent.extent_map;
      return os;
    }
};

typedef std::vector<ReadExtent> ReadExtents;

typedef std::map<uint64_t, uint64_t> ExtentMap;

} // namespace io
} // namespace librbd

#endif // CEPH_LIBRBD_IO_TYPES_H
