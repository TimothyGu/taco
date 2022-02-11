#ifndef TACO_LEGION_INCLUDES_H
#define TACO_LEGION_INCLUDES_H

#include "legion.h"
#include "mappers/default_mapper.h"
#include "legion_tensor.h"
#include "error.h"
#include <tuple>

// Fields used by the generated TACO code.
enum TensorFields {
  FID_VAL,
  FID_RECT_1,
  FID_COORD,
  FID_POINT_1,
};
const int TACO_TASK_BASE_ID = 10000;
const int TACO_SHARD_BASE_ID = 1000;

// A helper widget to treat LogicalRegions and PhysicalRegions the same.
struct RegionWrapper {
  Legion::PhysicalRegion physReg;
  Legion::LogicalRegion logReg;
  enum RegionKind {
    // UNSET is used when we default initialize a RegionWrapper without
    // a logical or a physical region.
    UNSET,
    PHYSICAL,
    LOGICAL,
  } regionKind;

  RegionWrapper() : regionKind(UNSET) {}
  RegionWrapper(Legion::LogicalRegion logReg) : logReg(logReg), regionKind(LOGICAL) {}
  RegionWrapper(Legion::PhysicalRegion physReg) : physReg(physReg), regionKind(PHYSICAL) {}
  Legion::IndexSpace get_index_space() {
    switch (this->regionKind) {
      case PHYSICAL:
        return this->physReg.get_logical_region().get_index_space();
      case LOGICAL:
        return this->logReg.get_index_space();
      default:
        taco_iassert(false);
        return Legion::IndexSpace(); // Keep the compiler happy.
    }
  }

  // We purposely don't make these `explicit` so that we don't have to generate code
  // that performs the desired casts. For now, the overload resolution has been enough.
  operator Legion::PhysicalRegion() {
    // If we aren't a physical region yet, then return an invalid PhysicalRegion
    // to be explicit that we are sending invalid data.
    switch (this->regionKind) {
      case LOGICAL:
        return Legion::PhysicalRegion();
      case PHYSICAL:
        return this->physReg;
      case UNSET:
        return Legion::PhysicalRegion();
    }
    taco_iassert(false) << "unreachable";
    return Legion::PhysicalRegion(); // Keep the compiler happy.
  }

  operator Legion::LogicalRegion() {
    // If we're a PhysicalRegion, then we can easily return the corresponding LogicalRegion.
    switch (this->regionKind) {
      case LOGICAL:
        return this->logReg;
      case PHYSICAL:
        return this->physReg.get_logical_region();
      case UNSET:
        return Legion::LogicalRegion();
    }
    taco_iassert(false) << "unreachable";
    return Legion::LogicalRegion(); // Keep the compiler happy.
  }
};

Legion::IndexSpace get_index_space(Legion::PhysicalRegion r);
Legion::IndexSpace get_index_space(Legion::LogicalRegion r);
Legion::IndexSpace get_index_space(RegionWrapper r);
Legion::LogicalRegion get_logical_region(Legion::PhysicalRegion r);
Legion::LogicalRegion get_logical_region(Legion::LogicalRegion r);
Legion::LogicalRegion get_logical_region(RegionWrapper r);
Legion::IndexPartition get_index_partition(Legion::IndexPartition i);
Legion::IndexPartition get_index_partition(Legion::LogicalPartition l);
int getIndexPoint(const Legion::Task* task, int index);
Legion::TaskID taskID(int offset);
Legion::ShardingID shardingID(int offset);

// createFieldSpaceWithSize initializes a field space with a single field
// of the given size.
Legion::FieldSpace createFieldSpaceWithSize(Legion::Context ctx, Legion::Runtime* runtime, Legion::FieldID id, size_t size);

// createSimpleDomain creates a domain with from zero to upper.
template<int DIM>
Legion::DomainT<DIM> createSimpleDomain(Legion::Point<DIM> upper) {
  auto lower = Legion::Point<DIM>::ZEROES();
  return Legion::Rect<DIM>(lower, upper);
}

// registerTacoRuntimeLibTasks registers all tasks that may be used by
// the generated code at runtime. The registered tasks are not tasks
// generated by DISTAL.
void registerTacoRuntimeLibTasks();
void registerPlacementShardingFunctor(Legion::Context ctx, Legion::Runtime* runtime, Legion::ShardingID funcID, std::vector<int>& dims);

// TODO (rohany): These might need to be templated on the dimension of the region.
// Functions for performing allocations on a region.
// TODO (rohany): Do these need to all take in parents and target regions?
// Allocate the entirety of a region with a given parent.
Legion::PhysicalRegion legionMalloc(Legion::Context ctx, Legion::Runtime* runtime, Legion::LogicalRegion region, Legion::LogicalRegion parent, Legion::FieldID fid, Legion::PrivilegeMode priv);
// Allocate a subregion from a region of the given size, i.e. [0, size).
Legion::PhysicalRegion legionMalloc(Legion::Context ctx, Legion::Runtime* runtime, Legion::LogicalRegion region, size_t size, Legion::FieldID fid, Legion::PrivilegeMode priv);
// Allocate a subregion from a region of exactly the given domain.
Legion::PhysicalRegion legionMalloc(Legion::Context ctx, Legion::Runtime* runtime, Legion::LogicalRegion region, Legion::Domain domain, Legion::FieldID fid, Legion::PrivilegeMode priv);
// Allocate a subregion from a region of a given size extended from an old size.
Legion::PhysicalRegion legionRealloc(Legion::Context ctx, Legion::Runtime* runtime, Legion::LogicalRegion region, Legion::PhysicalRegion old, size_t newSize, Legion::FieldID fid, Legion::PrivilegeMode priv);

// getSubRegion returns the LogicalRegion corresponding to the subregion of region with bounds.
Legion::LogicalRegion getSubRegion(Legion::Context ctx, Legion::Runtime* runtime, Legion::LogicalRegion region, Legion::Domain bounds);

// Copy a partition onto a region with the same index space.
Legion::LogicalPartition copyPartition(Legion::Context ctx, Legion::Runtime* runtime, Legion::IndexPartition toCopy, Legion::LogicalRegion toPartition, Legion::Color color = LEGION_AUTO_GENERATE_ID);
Legion::LogicalPartition copyPartition(Legion::Context ctx, Legion::Runtime* runtime, Legion::LogicalPartition toCopy, Legion::LogicalRegion toPartition, Legion::Color color = LEGION_AUTO_GENERATE_ID);
Legion::IndexPartition copyPartition(Legion::Context ctx, Legion::Runtime* runtime, Legion::IndexPartition toCopy, Legion::IndexSpace toPartition, Legion::Color color = LEGION_AUTO_GENERATE_ID);
Legion::IndexPartition copyPartition(Legion::Context ctx, Legion::Runtime* runtime, Legion::LogicalPartition toCopy, Legion::IndexSpace toPartition, Legion::Color color = LEGION_AUTO_GENERATE_ID);

// densifyPartition creates a partition where the domain of each subregion in part is converted
// to the bounding box of the partition. This function is currently only implemented for
// 1-dimensional color spaces.
Legion::IndexPartition densifyPartition(Legion::Context ctx, Legion::Runtime* runtime, Legion::IndexSpace ispace, Legion::IndexPartition part, Legion::Color color = LEGION_AUTO_GENERATE_ID);

// Create an IndexPartition where each color maps to the boundary overlap between adjacent colors.
Legion::IndexPartition createSparseAliasingPartitions(Legion::Context ctx, Legion::Runtime* runtime, Legion::IndexSpace ispace, Legion::IndexPartition part);

// Templated helper functions to potentially create accessors. These allow us to generate
// accessors when we don't have valid PhysicalRegions without running into problems.
template<typename T>
T createAccessor(Legion::PhysicalRegion& r, Legion::FieldID fid) {
  if (r.is_valid()) {
    return T(r, fid);
  }
  return T();
}
template<typename T>
T createAccessor(Legion::PhysicalRegion&& r, Legion::FieldID fid) {
  if (r.is_valid()) {
    return T(r, fid);
  }
  return T();
}
template<typename T>
T createAccessor(Legion::PhysicalRegion& r, Legion::FieldID fid, Legion::ReductionOpID redop) {
  if (r.is_valid()) {
    return T(r, fid, redop);
  }
  return T();
}
template<typename T>
T createAccessor(Legion::PhysicalRegion&& r, Legion::FieldID fid, Legion::ReductionOpID redop) {
  if (r.is_valid()) {
    return T(r, fid, redop);
  }
  return T();
}

// getPreviousPoint is a utility function to get the point lexicographically
// before the current point in the current rectangle. It is the inverse of
// the step function defined in legion/runtime/realm/point.inl.
template<int DIM>
#if defined (__CUDACC__)
__host__ __device__
#endif
Legion::Point<DIM> getPreviousPoint(Legion::Point<DIM> point, Legion::Rect<DIM> bounds) {
  for (int i = DIM - 1; i >= 0; i--) {
    // If this dimension of the point is greater than the lower bound,
    // subtract it and return.
    if (point[i] > bounds.lo[i]) {
      point[i]--;
      return point;
    }
    // Otherwise if the dimension is already at the lowest value, set it
    // to the upper bound and continue to smaller dimensions.
    point[i] = bounds.hi[i];
  }
  return point;
}

// AffineProjection represents a projection between two dense index space partitions
// as a function f that maps indices in one index space to indices in another.
class AffineProjection {
public:
  // Create an AffineProjection from a vector of indices, where the projs[i]
  // is mapped location for index i.
  AffineProjection(std::vector<int> projs);
  template <typename... Projs>
  AffineProjection(const Projs&... projs) : AffineProjection(std::vector<int>{projs...}) {}
  // Get the input and output dimension sizes of the projection.
  int dim() const;
  // Access the mapped index of this projection.
  int operator[](size_t i) const;
  // Apply the projection to an index partition.
  Legion::IndexPartition apply(Legion::Context ctx, Legion::Runtime* runtime, Legion::IndexPartition part, Legion::IndexSpace ispace, Legion::Color color = LEGION_AUTO_GENERATE_ID);
  // Apply the projection to a domain point. outputBounds contains the corresponding
  // bounds for the output point (i.e. zero, n). It is used when positions in the
  // output are not specified by the AffineProjection, as in situations where a lower
  // dimensional space is projected onto a higher dimensional space. For dimensions that
  // are not specified by the projection, they take the value given by outputBounds at
  // that dimension.
  Legion::DomainPoint apply(Legion::DomainPoint point, Legion::DomainPoint outputBounds);
  // Value to represent the \bot value of projection, i.e. an index
  // that does not map to other indices.
  const static int BOT;
private:
  std::vector<int> projs;
};

// Helper class to perform partitions for the downwards partition path for
// RectCompressed modes. It has a special case to avoid an element-wise operation
// for single-dimensional position regions.
class RectCompressedPosPartitionDownwards {
public:
  static Legion::IndexPartition
  apply(Legion::Context ctx, Legion::Runtime *runtime, Legion::IndexSpace ispace, Legion::LogicalPartition part,
        Legion::LogicalRegion parent, Legion::FieldID fid, Legion::Color color = LEGION_AUTO_GENERATE_ID);

  // Register all tasks used by the operation.
  static void registerTasks();
private:
  using Accessor = Legion::FieldAccessor<READ_ONLY, Legion::Rect<1>, 1, Legion::coord_t, Realm::AffineAccessor<Legion::Rect<1>, 1, Legion::coord_t>>;
  static Legion::Domain task(const Legion::Task* task, const std::vector<Legion::PhysicalRegion>& regions, Legion::Context ctx, Legion::Runtime* runtime);
#ifdef TACO_USE_CUDA
  static Legion::Domain gputask(const Legion::Task* task, const std::vector<Legion::PhysicalRegion>& regions, Legion::Context ctx, Legion::Runtime* runtime);
#endif
  static const int taskID;
};

// RectCompressedCoordinatePartition creates a partition of a RectCompressedModeFormat
// given a coloring of the dimension's coordinate space.
class RectCompressedCoordinatePartition {
public:
  // TODO (rohany): I'm not sure yet whether this should operate on the full region or a subset only.
  //  It's likely that this isn't something we have to support right now though (i.e. nested position space distributions),
  //  so we can probably punt on it to later.
  static Legion::LogicalPartition
  apply(Legion::Context ctx, Legion::Runtime *runtime, Legion::LogicalRegion region, Legion::LogicalRegion parent,
        Legion::FieldID fid, Legion::DomainPointColoring buckets, Legion::IndexSpace colorSpace,
        Legion::Color color = LEGION_AUTO_GENERATE_ID);
  static void registerTasks();
private:
  template<typename T, Legion::PrivilegeMode MODE>
  using Accessor = Legion::FieldAccessor<MODE, T, 1, Legion::coord_t, Realm::AffineAccessor<T, 1, Legion::coord_t>>;
  static void task(const Legion::Task* task, const std::vector<Legion::PhysicalRegion>& regions, Legion::Context ctx, Legion::Runtime* runtime);
  static const int taskID;
};

// SparseGatherProjection represents a projection from a sparse level's coordinates
// into coordinates of a dense level. This allows for position space loops over
// a sparse level to select only the subtrees of tensors that will actually be
// referenced in a computation.
class SparseGatherProjection {
public:
  // SparseGatherProjection initializes a projection with a target dimension
  // to map coordinates to.
  SparseGatherProjection(int mapTo);

  // Apply applies the projection from a LogicalPartition of a region to
  // a target index space.
  Legion::IndexPartition
  apply(Legion::Context ctx, Legion::Runtime *runtime, Legion::LogicalRegion reg, Legion::LogicalPartition part,
        Legion::FieldID fieldID, Legion::IndexSpace toPartition, Legion::Color color = LEGION_AUTO_GENERATE_ID);

  // Register the tasks used by the projection.
  static void registerTasks();
private:
  // Stores the target dimension for the projection.
  int mapTo;
  // A pack of arguments used by the projecting task.
  struct taskArgs {
    int mapTo;
    Legion::IndexSpace target;
    Legion::FieldID fieldID;
  };
  // The body of the task, templated on the dimension of the
  // target index space.
  template<int DIM>
  static void taskBody(Legion::Context ctx, Legion::Runtime* runtime, taskArgs args, Legion::PhysicalRegion input, Legion::PhysicalRegion output);
  // The intermediate task that performs the projection.
  static void task(const Legion::Task* task, const std::vector<Legion::PhysicalRegion>& regions, Legion::Context ctx, Legion::Runtime* runtime);
  static const int taskID;
};

// arrayEnd is inclusive (i.e. we must be able to access posArray[arrayEnd] safely.
// We'll also generate device code for this if we're going to run on GPUs.
template<typename T>
#if defined (__CUDACC__)
__host__ __device__
#endif
int64_t taco_binarySearchBefore(T posArray, int64_t arrayStart, int64_t arrayEnd, int32_t target) {
  if (posArray[arrayEnd].hi < target) {
    return arrayEnd;
  }
  // The binary search range must be exclusive.
  int lowerBound = arrayStart; // always <= target
  int upperBound = arrayEnd + 1; // always > target
  while (upperBound - lowerBound > 1) {
    // TOOD (rohany): Does this suffer from overflow?
    int mid = (upperBound + lowerBound) / 2;
    auto midRect = posArray[mid];
    // Contains checks lo <= target <= hi.
    if (midRect.contains(target)) { return mid; }
    // Either lo > target or target > hi.
    else if (target > midRect.hi) { lowerBound = mid; }
    // At this point, only lo > target.
    else { upperBound = mid; }
  }
  return lowerBound;
}

template<typename T>
#if defined (__CUDACC__)
__host__ __device__
#endif
int64_t taco_binarySearchAfter(T array, int64_t arrayStart, int64_t arrayEnd, int32_t target) {
  if (array[arrayStart] >= target) {
    return arrayStart;
  }
  int lowerBound = arrayStart; // always < target
  int upperBound = arrayEnd; // always >= target
  while (upperBound - lowerBound > 1) {
    int mid = (upperBound + lowerBound) / 2;
    int midValue = array[mid];
    if (midValue < target) {
      lowerBound = mid;
    } else if (midValue > target) {
      upperBound = mid;
    } else {
      return mid;
    }
  }
  return upperBound;
}

// A set of tasks and methods for distributed-parallel construction of sparse tensors.
class RectCompressedFinalizeYieldPositions : public Legion::IndexLauncher {
public:
  static void compute(Legion::Context ctx, Legion::Runtime* runtime, Legion::LogicalRegion region, Legion::LogicalPartition partition, Legion::FieldID fid);
  static void registerTasks();
private:
  // Local typedef for Accessors.
  template<int DIM, Legion::PrivilegeMode MODE>
  using Accessor = Legion::FieldAccessor<MODE, Legion::Rect<1>, DIM, Legion::coord_t, Realm::AffineAccessor<Legion::Rect<1>, DIM, Legion::coord_t>>;
  template<int DIM>
  static void body(Legion::Context ctx, Legion::Runtime *runtime,
                   Legion::Rect<DIM> fullBounds, Legion::Rect<DIM> iterBounds,
                   Accessor<DIM, READ_WRITE> output, Accessor<DIM, READ_ONLY> ghost);
  static void task(const Legion::Task* task, const std::vector<Legion::PhysicalRegion>& regions, Legion::Context ctx, Legion::Runtime* runtime);
#ifdef TACO_USE_CUDA
  static void gputask(const Legion::Task* task, const std::vector<Legion::PhysicalRegion>& regions, Legion::Context ctx, Legion::Runtime* runtime);
#endif
  static const int taskID;
};

class RectCompressedGetSeqInsertEdges {
public:
  struct ResultValuePack {
    int64_t scanResult;
    Legion::LogicalPartition partition;
  };
  static ResultValuePack compute(Legion::Context ctx, Legion::Runtime *runtime,
                                 Legion::IndexSpace colorSpace,
                                 Legion::LogicalRegion pos, Legion::FieldID posFid,
                                 Legion::LogicalRegion nnz, Legion::FieldID nnzFid);

  static void registerTasks();
private:
  // Local typedef for Accessors.
  template<typename T, int DIM, Legion::PrivilegeMode MODE>
  using Accessor = Legion::FieldAccessor<MODE, T, DIM, Legion::coord_t, Realm::AffineAccessor<T, DIM, Legion::coord_t>>;

  // TODO (rohany): I think that the best way of doing this is to have subclasses for each
  //  of these methods that inherit from IndexLauncher. However, each is only used once
  //  so it is not too much overhead.

  // There are two phases to the scan computation. A local scan on each processor,
  // and then an application of partial results onto each processor.

  // scan{Body,Task} are the implementations of the first step.
  template<int DIM>
  static int64_t scanBody(Legion::Context ctx, Legion::Runtime *runtime, Legion::Rect<DIM> iterBounds, Accessor<Legion::Rect<1>, DIM, WRITE_ONLY> output, Accessor<int64_t, DIM, READ_ONLY> input, Legion::Memory::Kind tmpMemKind);
  static int64_t scanTask(const Legion::Task* task, const std::vector<Legion::PhysicalRegion>& regions, Legion::Context ctx, Legion::Runtime* runtime);
  static std::pair<Legion::FieldID, Legion::FieldID> unpackScanTaskArgs(const Legion::Task* task);
#ifdef TACO_USE_CUDA
  template<int DIM>
  static int64_t scanBodyGPU(Legion::Context ctx, Legion::Runtime *runtime, Legion::Rect<DIM> iterBounds, Accessor<Legion::Rect<1>, DIM, WRITE_ONLY> output, Accessor<int64_t, DIM, READ_ONLY> input, Legion::Memory::Kind tmpMemKind);
  static int64_t scanTaskGPU(const Legion::Task* task, const std::vector<Legion::PhysicalRegion>& regions, Legion::Context ctx, Legion::Runtime* runtime);
#endif
  static const int scanTaskID;

  // applyPartialResults{Body,Task} are the implementations of the second step.
  template<int DIM>
  static void applyPartialResultsBody(Legion::Context ctx, Legion::Runtime *runtime, Legion::Rect<DIM> iterBounds, Accessor<Legion::Rect<1>, DIM, READ_WRITE> output, int64_t value);
  static void applyPartialResultsTask(const Legion::Task* task, const std::vector<Legion::PhysicalRegion>& regions, Legion::Context ctx, Legion::Runtime* runtime);
  static std::pair<Legion::FieldID, int64_t> unpackApplyPartialResultsTaskArgs(const Legion::Task* task);
#ifdef TACO_USE_CUDA
  static void applyPartialResultsTaskGPU(const Legion::Task* task, const std::vector<Legion::PhysicalRegion>& regions, Legion::Context ctx, Legion::Runtime* runtime);
#endif
  static const int applyPartialResultsTaskID;
};

#endif // TACO_LEGION_INCLUDES_H
