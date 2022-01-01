#ifndef TACO_LEGION_INCLUDES_H
#define TACO_LEGION_INCLUDES_H

#include "legion.h"
#include "mappers/default_mapper.h"
#include "legion_tensor.h"
#include "error.h"

// Fields used by the generated TACO code.
enum TensorFields {
  FID_VAL,
  FID_RECT_1,
  FID_COORD,
};
const int TACO_TASK_BASE_ID = 10000;
const int TACO_SHARD_BASE_ID = 1000;

// A helper widget to treat LogicalRegions and PhysicalRegions the same.
struct RegionWrapper {
  Legion::PhysicalRegion physReg;
  Legion::LogicalRegion logReg;
  enum RegionKind {
    PHYSICAL,
    LOGICAL,
  } regionKind;

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
    if (this->regionKind == LOGICAL) {
      return Legion::PhysicalRegion();
    }
    return this->physReg;
  }
  operator Legion::LogicalRegion() {
    // If we're a PhysicalRegion, then we can easily return the corresponding LogicalRegion.
    if (this->regionKind == PHYSICAL) {
      return this->physReg.get_logical_region();
    }
    return this->logReg;
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

void registerPlacementShardingFunctor(Legion::Context ctx, Legion::Runtime* runtime, Legion::ShardingID funcID, std::vector<int>& dims);

// TODO (rohany): These might need to be templated on the dimension of the region.
// Functions for performing allocations on a region.
// TODO (rohany): Do these need to all take in parents and target regions?
// Allocate the entirety of a region with a given parent.
Legion::PhysicalRegion legionMalloc(Legion::Context ctx, Legion::Runtime* runtime, Legion::LogicalRegion region, Legion::LogicalRegion parent, Legion::FieldID fid);
// Allocate a subregion from a region of the given size, i.e. [0, size).
Legion::PhysicalRegion legionMalloc(Legion::Context ctx, Legion::Runtime* runtime, Legion::LogicalRegion region, size_t size, Legion::FieldID fid);
// Allocate a subregion from a region of a given size extended from an old size.
Legion::PhysicalRegion legionRealloc(Legion::Context ctx, Legion::Runtime* runtime, Legion::LogicalRegion region, Legion::PhysicalRegion old, size_t newSize, Legion::FieldID fid);

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

// Affine project represents a projection between two dense index space partitions
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

// TODO (rohany): Double check these integer types.
// arrayEnd is inclusive (i.e. we must be able to access posArray[arrayEnd] safely.
// We'll also generate device code for this if we're going to run on GPUs.
template<typename T>
#if defined (__CUDACC__)
__host__ __device__
#endif
int taco_binarySearchBefore(T posArray, int arrayStart, int arrayEnd, int target) {
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

#endif // TACO_LEGION_INCLUDES_H
