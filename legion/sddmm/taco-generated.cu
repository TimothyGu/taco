#include "taco_legion_header.h"
#include "taco_mapper.h"
#define TACO_MIN(_a,_b) ((_a) < (_b) ? (_a) : (_b))
#define TACO_MAX(_a,_b) ((_a) < (_b) ? (_b) : (_a))
using namespace Legion;

#include "taco-generated.cuh"
#include "cublas_v2.h"
#include "cudalibs.h"
#include "leaf_kernels.cuh"
typedef FieldAccessor<READ_ONLY,double,1,coord_t,Realm::AffineAccessor<double,1,coord_t>> AccessorROdouble1;
typedef FieldAccessor<READ_ONLY,double,2,coord_t,Realm::AffineAccessor<double,2,coord_t>> AccessorROdouble2;
typedef FieldAccessor<READ_WRITE,double,1,coord_t,Realm::AffineAccessor<double,1,coord_t>> AccessorRWdouble1;
typedef FieldAccessor<READ_ONLY,int32_t,1,coord_t,Realm::AffineAccessor<int32_t,1,coord_t>> AccessorROint32_t1;
typedef FieldAccessor<READ_ONLY,Rect<1>,1,coord_t,Realm::AffineAccessor<Rect<1>,1,coord_t>> AccessorRORect_1_1;

struct task_1Args {
  int64_t B2Size;
  int32_t C2_dimension;
  int32_t pieces;
};


partitionPackForcomputeLegion partitionForcomputeLegion(Legion::Context ctx, Legion::Runtime* runtime, LegionTensor* A, LegionTensor* B, LegionTensor* C, LegionTensor* D, int32_t pieces) {
  auto A2_pos_parent = A->indicesParents[1][0];
  auto A2_crd_parent = A->indicesParents[1][1];
  RegionWrapper A_vals = A->vals;
  IndexSpace A_dense_run_0 = A->denseLevelRuns[0];
  RegionWrapper B2_pos = B->indices[1][0];
  RegionWrapper B2_crd = B->indices[1][1];
  auto B2_pos_parent = B->indicesParents[1][0];
  RegionWrapper B_vals = B->vals;
  IndexSpace B_dense_run_0 = B->denseLevelRuns[0];
  RegionWrapper C_vals = C->vals;
  IndexSpace C_dense_run_0 = C->denseLevelRuns[0];

  int64_t B2Size = runtime->get_index_space_domain(ctx, get_index_space(B2_crd)).hi()[0] + 1;

  Point<1> lowerBound = Point<1>(0);
  Point<1> upperBound = Point<1>((pieces - 1));
  auto fposoIndexSpace = runtime->create_index_space(ctx, Rect<1>(lowerBound, upperBound));
  DomainT<1> domain = runtime->get_index_space_domain(ctx, IndexSpaceT<1>(fposoIndexSpace));
  DomainT<1> B2_crd_domain = runtime->get_index_space_domain(ctx, B2_crd.get_index_space());
  DomainPointColoring B2_crd_coloring = DomainPointColoring();
  for (PointInDomainIterator<1> itr = PointInDomainIterator<1>(domain); itr.valid(); itr++) {
    int32_t fposo = (*itr)[0];
    Point<1> B2CrdStart = Point<1>((fposo * ((B2Size + (pieces - 1)) / pieces)));
    Point<1> B2CrdEnd = Point<1>(TACO_MIN((fposo * ((B2Size + (pieces - 1)) / pieces) + ((B2Size + (pieces - 1)) / pieces - 1)),B2_crd_domain.bounds.hi[0]));
    Rect<1> B2CrdRect = Rect<1>(B2CrdStart, B2CrdEnd);
    if (!B2_crd_domain.contains(B2CrdRect.lo) || !B2_crd_domain.contains(B2CrdRect.hi)) {
      B2CrdRect = B2CrdRect.make_empty();
    }
    B2_crd_coloring[(*itr)] = B2CrdRect;
  }
  IndexPartition B2_crd_index_part = runtime->create_index_partition(ctx, B2_crd.get_index_space(), domain, B2_crd_coloring, LEGION_COMPUTE_KIND);
  LogicalPartition B2_crd_part = runtime->get_logical_partition(ctx, B2_crd, B2_crd_index_part);
  IndexPartition posSparsePartB2 = runtime->create_partition_by_preimage_range(
    ctx,
    B2_crd_index_part,
    B2_pos,
    B2_pos_parent,
    FID_RECT_1,
    runtime->get_index_partition_color_space_name(ctx, B2_crd_index_part)
  );
  IndexPartition posIndexPartB2 = densifyPartition(ctx, runtime, get_index_space(B2_pos), posSparsePartB2);
  LogicalPartition posPartB2 = runtime->get_logical_partition(ctx, B2_pos, posIndexPartB2);
  LogicalPartition BValsLogicalPart = copyPartition(ctx, runtime, B2_crd_part, B_vals);
  IndexPartition BDenseRun0Partition = copyPartition(ctx, runtime, posPartB2, B_dense_run_0);
  LogicalPartition AValsLogicalPart = copyPartition(ctx, runtime, B2_crd_part, A_vals);
  LogicalPartition posPartA2 = copyPartition(ctx, runtime, posPartB2, A2_pos_parent);
  LogicalPartition crdPartA2 = copyPartition(ctx, runtime, B2_crd_part, A2_crd_parent);
  IndexPartition ADenseRun0Partition = copyPartition(ctx, runtime, posPartA2, A_dense_run_0);
  IndexPartition CDenseRun0Partition = AffineProjection(0).apply(ctx, runtime, BDenseRun0Partition, C_dense_run_0);
  auto C_vals_partition = copyPartition(ctx, runtime, CDenseRun0Partition, get_logical_region(C_vals));
  auto computePartitions = partitionPackForcomputeLegion();
  computePartitions.APartition.indicesPartitions = std::vector<std::vector<LogicalPartition>>(2);
  computePartitions.APartition.denseLevelRunPartitions = std::vector<IndexPartition>(2);
  computePartitions.APartition.indicesPartitions[1].push_back(posPartA2);
  computePartitions.APartition.indicesPartitions[1].push_back(crdPartA2);
  computePartitions.APartition.valsPartition = AValsLogicalPart;
  computePartitions.APartition.denseLevelRunPartitions[0] = ADenseRun0Partition;
  computePartitions.BPartition.indicesPartitions = std::vector<std::vector<LogicalPartition>>(2);
  computePartitions.BPartition.denseLevelRunPartitions = std::vector<IndexPartition>(2);
  computePartitions.BPartition.indicesPartitions[1].push_back(posPartB2);
  computePartitions.BPartition.indicesPartitions[1].push_back(B2_crd_part);
  computePartitions.BPartition.valsPartition = BValsLogicalPart;
  computePartitions.BPartition.denseLevelRunPartitions[0] = BDenseRun0Partition;
  computePartitions.CPartition.indicesPartitions = std::vector<std::vector<LogicalPartition>>(2);
  computePartitions.CPartition.denseLevelRunPartitions = std::vector<IndexPartition>(2);
  computePartitions.CPartition.valsPartition = C_vals_partition;
  computePartitions.CPartition.denseLevelRunPartitions[0] = CDenseRun0Partition;

  return computePartitions;
}

__global__
void task_1DeviceKernel0(int64_t B2Size, int32_t* i_blockStarts, int32_t pieces, AccessorRORect_1_1 B2_pos_accessor, AccessorROint32_t1 B2_crd_accessor, AccessorRWdouble1 A_vals_rw_accessor, AccessorROdouble1 B_vals_ro_accessor, AccessorROdouble2 C_vals_ro_accessor, AccessorROdouble2 D_vals_ro_accessor, int32_t C2_dimension, int32_t fposo) {

  int32_t block = blockIdx.x;
  int32_t thread = (threadIdx.x % (32));
  int32_t warp = (threadIdx.x / 32);
  if (threadIdx.x >= 256) {
    return;
  }

  int64_t pointID2 = fposo * (((B2Size + (pieces - 1)) / pieces + 2047) / 2048) + block;
  int64_t pointID3 = pointID2 * 8 + warp;
  int32_t pB2_begin = i_blockStarts[block];
  int32_t pB2_end = i_blockStarts[(block + 1)];
  int32_t fposi1 = warp * 256;
  int32_t fposi = block * 2048 + fposi1;
  int32_t fposB = fposo * ((B2Size + (pieces - 1)) / pieces) + fposi;
  int32_t i_pos = taco_binarySearchBefore(B2_pos_accessor, pB2_begin, pB2_end, fposB);
  int32_t i = i_pos;
  for (int32_t nnz = 0; nnz < 256; nnz++) {
    int32_t fposi1 = warp * 256 + nnz;
    int32_t fposi = block * 2048 + fposi1;
    int32_t fposB = fposo * ((B2Size + (pieces - 1)) / pieces) + fposi;
    if (fposB >= (fposo + 1) * ((B2Size + (pieces - 1)) / pieces))
      break;

    if (fposB >= B2Size)
      break;

    int32_t f = B2_crd_accessor[fposB];
    while (!(B2_pos_accessor[i_pos].contains(fposB))) {
      i_pos = i_pos + 1;
      i = i_pos;
    }
    int64_t pointID4 = pointID3 * 32 + thread;
    for (int32_t dense_b = 0; dense_b < ((C2_dimension + 31) / 32); dense_b++) {
      int32_t j = dense_b * 32 + thread;
      if (j >= C2_dimension)
        break;

      atomicAddWarp(A_vals_rw_accessor.ptr(Point<1>(fposB)), flattenPoint(A_vals_rw_accessor, Point<1>(fposB)), ((B_vals_ro_accessor[Point<1>(fposB)] * C_vals_ro_accessor[Point<2>(i, j)]) * D_vals_ro_accessor[Point<2>(j, f)]));
    }
  }
}

void task_1(const Task* task, const std::vector<PhysicalRegion>& regions, Context ctx, Runtime* runtime) {
  PhysicalRegion A2_pos = regions[0];
  LogicalRegion A2_pos_parent = regions[0].get_logical_region();
  PhysicalRegion A2_crd = regions[1];
  LogicalRegion A2_crd_parent = regions[1].get_logical_region();
  PhysicalRegion A_vals = regions[2];
  LogicalRegion A_vals_parent = regions[2].get_logical_region();
  PhysicalRegion B2_pos = regions[3];
  LogicalRegion B2_pos_parent = regions[3].get_logical_region();
  PhysicalRegion B2_crd = regions[4];
  LogicalRegion B2_crd_parent = regions[4].get_logical_region();
  PhysicalRegion B_vals = regions[5];
  LogicalRegion B_vals_parent = regions[5].get_logical_region();
  PhysicalRegion C_vals = regions[6];
  LogicalRegion C_vals_parent = regions[6].get_logical_region();
  PhysicalRegion D_vals = regions[7];
  LogicalRegion D_vals_parent = regions[7].get_logical_region();

  int32_t fposo = task->index_point[0];
  task_1Args* args = (task_1Args*)(task->args);
  int64_t B2Size = args->B2Size;
  int32_t C2_dimension = args->C2_dimension;
  int32_t pieces = args->pieces;

  auto B_vals_ro_accessor = createAccessor<AccessorROdouble1>(B_vals, FID_VAL);
  auto C_vals_ro_accessor = createAccessor<AccessorROdouble2>(C_vals, FID_VAL);
  auto D_vals_ro_accessor = createAccessor<AccessorROdouble2>(D_vals, FID_VAL);
  auto A_vals_rw_accessor = createAccessor<AccessorRWdouble1>(A_vals, FID_VAL);
  auto B2_pos_accessor = createAccessor<AccessorRORect_1_1>(B2_pos, FID_RECT_1);
  auto B2_crd_accessor = createAccessor<AccessorROint32_t1>(B2_crd, FID_COORD);

  if (runtime->get_index_space_domain(ctx, get_index_space(B2_crd)).empty())
    return ;

  DomainT<1> B2PosDomain = runtime->get_index_space_domain(ctx, get_index_space(B2_pos));
  DomainT<1> B2CrdDomain = runtime->get_index_space_domain(ctx, get_index_space(B2_crd));
  Legion::DeferredBuffer<int32_t, 1> buf = Legion::DeferredBuffer<int32_t, 1>(Rect<1>(0, (((B2Size + (pieces - 1)) / pieces + 2047) / 2048)), Legion::Memory::Kind::GPU_FB_MEM);
  int32_t* i_blockStarts = buf.ptr(0);
  taco_binarySearchBeforeBlockLaunch(
    B2_pos_accessor,
    i_blockStarts,
    B2PosDomain.bounds.lo,
    B2PosDomain.bounds.hi,
    2048,
    256,
    (((B2Size + (pieces - 1)) / pieces + 2047) / 2048),
    B2CrdDomain.bounds.lo
  );
  if (((((B2Size + (pieces - 1)) / pieces + 2047) / 2048)) > 0) {
    task_1DeviceKernel0<<<(((B2Size + (pieces - 1)) / pieces + 2047) / 2048), (32 * 8)>>>(B2Size, i_blockStarts, pieces, B2_pos_accessor, B2_crd_accessor, A_vals_rw_accessor, B_vals_ro_accessor, C_vals_ro_accessor, D_vals_ro_accessor, C2_dimension, fposo);
  }
}

void computeLegion(Legion::Context ctx, Legion::Runtime* runtime, LegionTensor* A, LegionTensor* B, LegionTensor* C, LegionTensor* D, partitionPackForcomputeLegion* partitionPack, int32_t pieces) {
  auto A2_pos_parent = A->indicesParents[1][0];
  auto A2_crd_parent = A->indicesParents[1][1];
  auto A_vals_parent = A->valsParent;
  RegionWrapper B2_crd = B->indices[1][1];
  auto B2_pos_parent = B->indicesParents[1][0];
  auto B2_crd_parent = B->indicesParents[1][1];
  auto B_vals_parent = B->valsParent;
  int C2_dimension = C->dims[1];
  auto C_vals_parent = C->valsParent;
  RegionWrapper D_vals = D->vals;
  auto D_vals_parent = D->valsParent;

  int64_t B2Size = runtime->get_index_space_domain(ctx, get_index_space(B2_crd)).hi()[0] + 1;

  Point<1> lowerBound = Point<1>(0);
  Point<1> upperBound = Point<1>((pieces - 1));
  auto fposoIndexSpace = runtime->create_index_space(ctx, Rect<1>(lowerBound, upperBound));
  DomainT<1> domain = runtime->get_index_space_domain(ctx, IndexSpaceT<1>(fposoIndexSpace));
  task_1Args taskArgsRaw1;
  taskArgsRaw1.B2Size = B2Size;
  taskArgsRaw1.C2_dimension = C2_dimension;
  taskArgsRaw1.pieces = pieces;
  TaskArgument taskArgs = TaskArgument(&taskArgsRaw1, sizeof(task_1Args));
  IndexLauncher launcher = IndexLauncher(taskID(1), domain, taskArgs, ArgumentMap());
  launcher.add_region_requirement(RegionRequirement(
    partitionPack->APartition.indicesPartitions[1][0],
    0,
    READ_ONLY,
    EXCLUSIVE,
    get_logical_region(A2_pos_parent),
    Mapping::DefaultMapper::VIRTUAL_MAP
  ).add_field(FID_RECT_1));
  launcher.add_region_requirement(RegionRequirement(
    partitionPack->APartition.indicesPartitions[1][1],
    0,
    READ_ONLY,
    EXCLUSIVE,
    get_logical_region(A2_crd_parent),
    Mapping::DefaultMapper::VIRTUAL_MAP
  ).add_field(FID_COORD));
  launcher.add_region_requirement(RegionRequirement(partitionPack->APartition.valsPartition, 0, READ_WRITE, EXCLUSIVE, A_vals_parent).add_field(FID_VAL));
  launcher.add_region_requirement(RegionRequirement(partitionPack->BPartition.indicesPartitions[1][0], 0, READ_ONLY, EXCLUSIVE, get_logical_region(B2_pos_parent)).add_field(FID_RECT_1));
  launcher.add_region_requirement(RegionRequirement(partitionPack->BPartition.indicesPartitions[1][1], 0, READ_ONLY, EXCLUSIVE, get_logical_region(B2_crd_parent)).add_field(FID_COORD));
  launcher.add_region_requirement(RegionRequirement(partitionPack->BPartition.valsPartition, 0, READ_ONLY, EXCLUSIVE, B_vals_parent).add_field(FID_VAL));
  launcher.add_region_requirement(RegionRequirement(partitionPack->CPartition.valsPartition, 0, READ_ONLY, EXCLUSIVE, C_vals_parent).add_field(FID_VAL));
  launcher.add_region_requirement(RegionRequirement(get_logical_region(D_vals), READ_ONLY, EXCLUSIVE, D_vals_parent).add_field(FID_VAL));
  launcher.tag = launcher.tag | TACOMapper::UNTRACK_VALID_REGIONS;
  runtime->execute_index_space(ctx, launcher);

}
void registerTacoTasks() {
  {
    TaskVariantRegistrar registrar(taskID(1), "task_1");
    registrar.add_constraint(ProcessorConstraint(Processor::TOC_PROC));
    registrar.set_leaf();
    Runtime::preregister_task_variant<task_1>(registrar, "task_1");
  }
}