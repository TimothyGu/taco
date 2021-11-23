#include "taco_legion_header.h"
#include "taco_mapper.h"
#define TACO_MIN(_a,_b) ((_a) < (_b) ? (_a) : (_b))
using namespace Legion;
typedef FieldAccessor<READ_ONLY,int32_t,1,coord_t,Realm::AffineAccessor<int32_t,1,coord_t>> AccessorROint32_t1;
typedef FieldAccessor<READ_ONLY,double,1,coord_t,Realm::AffineAccessor<double,1,coord_t>> AccessorROdouble1;
typedef FieldAccessor<READ_ONLY,Rect<1>,1,coord_t,Realm::AffineAccessor<Rect<1>,1,coord_t>> AccessorRORect_1_1;
typedef FieldAccessor<READ_WRITE,double,2,coord_t,Realm::AffineAccessor<double,2,coord_t>> AccessorRWdouble2;
typedef FieldAccessor<READ_ONLY,Rect<1>,2,coord_t,Realm::AffineAccessor<Rect<1>,2,coord_t>> AccessorRORect_1_2;

struct partitionPackForcomputeLegionDSS {
  LegionTensorPartition aPartition;
  LegionTensorPartition BPartition;
};

struct task_1Args {
  int32_t B1_dimension;
  int32_t a1_dimension;
  int32_t a2_dimension;
  int32_t c1_dimension;
  int32_t gx;
};
struct partitionPackForcomputeLegionDDS {
  LegionTensorPartition aPartition;
  LegionTensorPartition BPartition;
};

struct task_2Args {
  int32_t B1_dimension;
  int32_t B2_dimension;
  int32_t a1_dimension;
  int32_t a2_dimension;
  int32_t c1_dimension;
  int32_t gx;
  int32_t gy;
};

partitionPackForcomputeLegionDSS* partitionForcomputeLegionDSS(Context ctx, Runtime* runtime, LegionTensor* a, LegionTensor* B, LegionTensor* c, int32_t gx) {
  RegionWrapper a_vals = a->vals;
  IndexSpace a_dense_run_0 = a->denseLevelRuns[0];
  int B1_dimension = B->dims[0];
  int B2_dimension = B->dims[1];
  RegionWrapper B2_pos = B->indices[1][0];
  RegionWrapper B2_crd = B->indices[1][1];
  RegionWrapper B3_pos = B->indices[2][0];
  RegionWrapper B3_crd = B->indices[2][1];
  auto B2_pos_parent = B->indicesParents[1][0];
  auto B2_crd_parent = B->indicesParents[1][1];
  auto B3_pos_parent = B->indicesParents[2][0];
  auto B3_crd_parent = B->indicesParents[2][1];
  RegionWrapper B_vals = B->vals;
  IndexSpace B_dense_run_0 = B->denseLevelRuns[0];
  IndexSpace c_dense_run_0 = c->denseLevelRuns[0];

  Point<1> lowerBound = Point<1>(0);
  Point<1> upperBound = Point<1>((gx - 1));
  auto ioIndexSpace = runtime->create_index_space(ctx, Rect<1>(lowerBound, upperBound));
  DomainT<1> domain = runtime->get_index_space_domain(ctx, IndexSpaceT<1>(ioIndexSpace));
  auto BDomain = runtime->get_index_space_domain(ctx, B_dense_run_0);
  auto aDomain = runtime->get_index_space_domain(ctx, a_dense_run_0);
  auto cDomain = runtime->get_index_space_domain(ctx, c_dense_run_0);
  DomainPointColoring BColoring = DomainPointColoring();
  DomainPointColoring aColoring = DomainPointColoring();
  DomainPointColoring cColoring = DomainPointColoring();
  for (PointInDomainIterator<1> itr = PointInDomainIterator<1>(domain); itr.valid(); itr++) {
    int32_t io = (*itr)[0];
    Point<1> BStart = Point<1>((io * ((B1_dimension + (gx - 1)) / gx) + 0 / gx));
    Point<1> BEnd = Point<1>(TACO_MIN((io * ((B1_dimension + (gx - 1)) / gx) + ((B1_dimension + (gx - 1)) / gx - 1)), BDomain.hi()[0]));
    Rect<1> BRect = Rect<1>(BStart, BEnd);
    if (!BDomain.contains(BRect.lo) || !BDomain.contains(BRect.hi)) {
      BRect = BRect.make_empty();
    }
    BColoring[(*itr)] = BRect;
    Point<2> aStart = Point<2>((io * ((B1_dimension + (gx - 1)) / gx) + 0 / gx), 0);
    Point<2> aEnd = Point<2>(TACO_MIN((io * ((B1_dimension + (gx - 1)) / gx) + ((B1_dimension + (gx - 1)) / gx - 1)), aDomain.hi()[0]), TACO_MIN(B2_dimension, aDomain.hi()[1]));
    Rect<2> aRect = Rect<2>(aStart, aEnd);
    if (!aDomain.contains(aRect.lo) || !aDomain.contains(aRect.hi)) {
      aRect = aRect.make_empty();
    }
    aColoring[(*itr)] = aRect;
  }
  auto BPartition = runtime->create_index_partition(ctx, B_dense_run_0, domain, BColoring, LEGION_DISJOINT_COMPLETE_KIND);
  LogicalPartition posPartB2 = copyPartition(ctx, runtime, BPartition, B2_pos);
  LogicalPartition crdPartB2 = runtime->get_logical_partition(ctx, B2_crd_parent, runtime->create_partition_by_image_range(
    ctx,
    B2_crd.get_index_space(),
    posPartB2,
    B2_pos_parent,
    FID_RECT_1,
    runtime->get_index_partition_color_space_name(ctx, posPartB2.get_index_partition())
  ));
  LogicalPartition posPartB3 = copyPartition(ctx, runtime, crdPartB2, B3_pos);
  LogicalPartition crdPartB3 = runtime->get_logical_partition(ctx, B3_crd_parent, runtime->create_partition_by_image_range(
    ctx,
    B3_crd.get_index_space(),
    posPartB3,
    B3_pos_parent,
    FID_RECT_1,
    runtime->get_index_partition_color_space_name(ctx, posPartB3.get_index_partition())
  ));
  auto B_vals_partition = copyPartition(ctx, runtime, crdPartB3, B_vals);
  auto aPartition = runtime->create_index_partition(ctx, a_dense_run_0, domain, aColoring, LEGION_DISJOINT_COMPLETE_KIND);
  auto a_vals_partition = copyPartition(ctx, runtime, aPartition, a_vals);
  auto computePartitions = new(partitionPackForcomputeLegionDSS);
  computePartitions->aPartition.indicesPartitions = std::vector<std::vector<LogicalPartition>>(2);
  computePartitions->aPartition.denseLevelRunPartitions = std::vector<IndexPartition>(2);
  computePartitions->aPartition.valsPartition = a_vals_partition;
  computePartitions->aPartition.denseLevelRunPartitions[0] = aPartition;
  computePartitions->BPartition.indicesPartitions = std::vector<std::vector<LogicalPartition>>(3);
  computePartitions->BPartition.denseLevelRunPartitions = std::vector<IndexPartition>(3);
  computePartitions->BPartition.indicesPartitions[1].push_back(posPartB2);
  computePartitions->BPartition.indicesPartitions[1].push_back(crdPartB2);
  computePartitions->BPartition.indicesPartitions[2].push_back(posPartB3);
  computePartitions->BPartition.indicesPartitions[2].push_back(crdPartB3);
  computePartitions->BPartition.valsPartition = B_vals_partition;
  computePartitions->BPartition.denseLevelRunPartitions[0] = BPartition;
  return computePartitions;
}

void task_1(const Task* task, const std::vector<PhysicalRegion>& regions, Context ctx, Runtime* runtime) {
  PhysicalRegion a_vals = regions[0];
  PhysicalRegion B2_pos = regions[1];
  PhysicalRegion B2_crd = regions[2];
  PhysicalRegion B3_pos = regions[3];
  PhysicalRegion B3_crd = regions[4];
  PhysicalRegion B_vals = regions[5];
  PhysicalRegion c_vals = regions[6];

  int32_t io = task->index_point[0];
  task_1Args* args = (task_1Args*)(task->args);
  int32_t B1_dimension = args->B1_dimension;
  int32_t a1_dimension = args->a1_dimension;
  int32_t a2_dimension = args->a2_dimension;
  int32_t c1_dimension = args->c1_dimension;
  int32_t gx = args->gx;

  auto c_vals_ro_accessor = createAccessor<AccessorROdouble1>(c_vals, FID_VAL);
  auto B_vals_ro_accessor = createAccessor<AccessorROdouble1>(B_vals, FID_VAL);
  auto a_vals_rw_accessor = createAccessor<AccessorRWdouble2>(a_vals, FID_VAL);
  auto B2_pos_accessor = createAccessor<AccessorRORect_1_1>(B2_pos, FID_RECT_1);
  auto B2_crd_accessor = createAccessor<AccessorROint32_t1>(B2_crd, FID_COORD);
  auto B3_pos_accessor = createAccessor<AccessorRORect_1_1>(B3_pos, FID_RECT_1);
  auto B3_crd_accessor = createAccessor<AccessorROint32_t1>(B3_crd, FID_COORD);

  #pragma omp parallel for schedule(static)
  for (int32_t ii = 0 / gx; ii < ((B1_dimension + (gx - 1)) / gx); ii++) {
    int32_t i = io * ((B1_dimension + (gx - 1)) / gx) + ii;
    if (i >= B1_dimension)
      continue;

    if (i >= (io + 1) * ((B1_dimension + (gx - 1)) / gx - 0 / gx))
      continue;

    for (int32_t jB = B2_pos_accessor[Point<1>(i)].lo; jB < (B2_pos_accessor[Point<1>(i)].hi + 1); jB++) {
      int32_t j = B2_crd_accessor[jB];
      for (int32_t kB = B3_pos_accessor[Point<1>(jB)].lo; kB < (B3_pos_accessor[Point<1>(jB)].hi + 1); kB++) {
        int32_t k = B3_crd_accessor[kB];
        a_vals_rw_accessor[Point<2>(i, j)] = a_vals_rw_accessor[Point<2>(i, j)] + B_vals_ro_accessor[Point<1>(kB)] * c_vals_ro_accessor[Point<1>(k)];
      }
    }
  }
}

void computeLegionDSS(Context ctx, Runtime* runtime, LegionTensor* a, LegionTensor* B, LegionTensor* c, partitionPackForcomputeLegionDSS* partitionPack, int32_t gx) {
  int a1_dimension = a->dims[0];
  int a2_dimension = a->dims[1];
  auto a_vals_parent = a->valsParent;
  int B1_dimension = B->dims[0];
  auto B2_pos_parent = B->indicesParents[1][0];
  auto B2_crd_parent = B->indicesParents[1][1];
  auto B3_pos_parent = B->indicesParents[2][0];
  auto B3_crd_parent = B->indicesParents[2][1];
  auto B_vals_parent = B->valsParent;
  int c1_dimension = c->dims[0];
  RegionWrapper c_vals = c->vals;
  auto c_vals_parent = c->valsParent;

  Point<1> lowerBound = Point<1>(0);
  Point<1> upperBound = Point<1>((gx - 1));
  auto ioIndexSpace = runtime->create_index_space(ctx, Rect<1>(lowerBound, upperBound));
  DomainT<1> domain = runtime->get_index_space_domain(ctx, IndexSpaceT<1>(ioIndexSpace));
  task_1Args taskArgsRaw;
  taskArgsRaw.B1_dimension = B1_dimension;
  taskArgsRaw.a1_dimension = a1_dimension;
  taskArgsRaw.a2_dimension = a2_dimension;
  taskArgsRaw.c1_dimension = c1_dimension;
  taskArgsRaw.gx = gx;
  TaskArgument taskArgs = TaskArgument(&taskArgsRaw, sizeof(task_1Args));
  IndexLauncher launcher = IndexLauncher(taskID(1), domain, taskArgs, ArgumentMap());
  launcher.add_region_requirement(RegionRequirement(partitionPack->aPartition.valsPartition, 0, READ_WRITE, EXCLUSIVE, a_vals_parent).add_field(FID_VAL));
  launcher.add_region_requirement(RegionRequirement(partitionPack->BPartition.indicesPartitions[1][0], 0, READ_ONLY, EXCLUSIVE, get_logical_region(B2_pos_parent)).add_field(FID_RECT_1));
  launcher.add_region_requirement(RegionRequirement(partitionPack->BPartition.indicesPartitions[1][1], 0, READ_ONLY, EXCLUSIVE, get_logical_region(B2_crd_parent)).add_field(FID_COORD));
  launcher.add_region_requirement(RegionRequirement(partitionPack->BPartition.indicesPartitions[2][0], 0, READ_ONLY, EXCLUSIVE, get_logical_region(B3_pos_parent)).add_field(FID_RECT_1));
  launcher.add_region_requirement(RegionRequirement(partitionPack->BPartition.indicesPartitions[2][1], 0, READ_ONLY, EXCLUSIVE, get_logical_region(B3_crd_parent)).add_field(FID_COORD));
  launcher.add_region_requirement(RegionRequirement(partitionPack->BPartition.valsPartition, 0, READ_ONLY, EXCLUSIVE, B_vals_parent).add_field(FID_VAL));
  launcher.add_region_requirement(RegionRequirement(c_vals, READ_ONLY, EXCLUSIVE, c_vals_parent).add_field(FID_VAL));
  runtime->execute_index_space(ctx, launcher);

}

partitionPackForcomputeLegionDDS* partitionForcomputeLegionDDS(Context ctx, Runtime* runtime, LegionTensor* a, LegionTensor* B, LegionTensor* c, int32_t gx, int32_t gy) {
  RegionWrapper a_vals = a->vals;
  IndexSpace a_dense_run_0 = a->denseLevelRuns[0];
  int B1_dimension = B->dims[0];
  int B2_dimension = B->dims[1];
  RegionWrapper B3_pos = B->indices[2][0];
  RegionWrapper B3_crd = B->indices[2][1];
  auto B3_pos_parent = B->indicesParents[2][0];
  auto B3_crd_parent = B->indicesParents[2][1];
  RegionWrapper B_vals = B->vals;
  IndexSpace B_dense_run_0 = B->denseLevelRuns[0];
  IndexSpace c_dense_run_0 = c->denseLevelRuns[0];

  Point<2> lowerBound = Point<2>(0, 0);
  Point<2> upperBound = Point<2>((gx - 1), (gy - 1));
  auto distFusedIndexSpace = runtime->create_index_space(ctx, Rect<2>(lowerBound, upperBound));
  DomainT<2> domain = runtime->get_index_space_domain(ctx, IndexSpaceT<2>(distFusedIndexSpace));
  auto BDomain = runtime->get_index_space_domain(ctx, B_dense_run_0);
  auto aDomain = runtime->get_index_space_domain(ctx, a_dense_run_0);
  auto cDomain = runtime->get_index_space_domain(ctx, c_dense_run_0);
  DomainPointColoring BColoring = DomainPointColoring();
  DomainPointColoring aColoring = DomainPointColoring();
  DomainPointColoring cColoring = DomainPointColoring();
  for (PointInDomainIterator<2> itr = PointInDomainIterator<2>(domain); itr.valid(); itr++) {
    int32_t io = (*itr)[0];
    int32_t jo = (*itr)[1];
    Point<2> BStart = Point<2>((io * ((B1_dimension + (gx - 1)) / gx) + 0 / gx), (jo * ((B2_dimension + (gy - 1)) / gy) + 0 / gy));
    Point<2> BEnd = Point<2>(TACO_MIN((io * ((B1_dimension + (gx - 1)) / gx) + ((B1_dimension + (gx - 1)) / gx - 1)), BDomain.hi()[0]), TACO_MIN((jo * ((B2_dimension + (gy - 1)) / gy) + ((B2_dimension + (gy - 1)) / gy - 1)), BDomain.hi()[1]));
    Rect<2> BRect = Rect<2>(BStart, BEnd);
    if (!BDomain.contains(BRect.lo) || !BDomain.contains(BRect.hi)) {
      BRect = BRect.make_empty();
    }
    BColoring[(*itr)] = BRect;
    Point<2> aStart = Point<2>((io * ((B1_dimension + (gx - 1)) / gx) + 0 / gx), (jo * ((B2_dimension + (gy - 1)) / gy) + 0 / gy));
    Point<2> aEnd = Point<2>(TACO_MIN((io * ((B1_dimension + (gx - 1)) / gx) + ((B1_dimension + (gx - 1)) / gx - 1)), aDomain.hi()[0]), TACO_MIN((jo * ((B2_dimension + (gy - 1)) / gy) + ((B2_dimension + (gy - 1)) / gy - 1)), aDomain.hi()[1]));
    Rect<2> aRect = Rect<2>(aStart, aEnd);
    if (!aDomain.contains(aRect.lo) || !aDomain.contains(aRect.hi)) {
      aRect = aRect.make_empty();
    }
    aColoring[(*itr)] = aRect;
  }
  auto BPartition = runtime->create_index_partition(ctx, B_dense_run_0, domain, BColoring, LEGION_DISJOINT_COMPLETE_KIND);
  LogicalPartition posPartB3 = copyPartition(ctx, runtime, BPartition, B3_pos);
  LogicalPartition crdPartB3 = runtime->get_logical_partition(ctx, B3_crd_parent, runtime->create_partition_by_image_range(
    ctx,
    B3_crd.get_index_space(),
    posPartB3,
    B3_pos_parent,
    FID_RECT_1,
    runtime->get_index_partition_color_space_name(ctx, posPartB3.get_index_partition())
  ));
  auto B_vals_partition = copyPartition(ctx, runtime, crdPartB3, B_vals);
  auto aPartition = runtime->create_index_partition(ctx, a_dense_run_0, domain, aColoring, LEGION_DISJOINT_COMPLETE_KIND);
  auto a_vals_partition = copyPartition(ctx, runtime, aPartition, a_vals);
  auto computePartitions = new(partitionPackForcomputeLegionDDS);
  computePartitions->aPartition.indicesPartitions = std::vector<std::vector<LogicalPartition>>(2);
  computePartitions->aPartition.denseLevelRunPartitions = std::vector<IndexPartition>(2);
  computePartitions->aPartition.valsPartition = a_vals_partition;
  computePartitions->aPartition.denseLevelRunPartitions[0] = aPartition;
  computePartitions->BPartition.indicesPartitions = std::vector<std::vector<LogicalPartition>>(3);
  computePartitions->BPartition.denseLevelRunPartitions = std::vector<IndexPartition>(3);
  computePartitions->BPartition.indicesPartitions[2].push_back(posPartB3);
  computePartitions->BPartition.indicesPartitions[2].push_back(crdPartB3);
  computePartitions->BPartition.valsPartition = B_vals_partition;
  computePartitions->BPartition.denseLevelRunPartitions[0] = BPartition;
  return computePartitions;
}

void task_2(const Task* task, const std::vector<PhysicalRegion>& regions, Context ctx, Runtime* runtime) {
  PhysicalRegion a_vals = regions[0];
  PhysicalRegion B3_pos = regions[1];
  PhysicalRegion B3_crd = regions[2];
  PhysicalRegion B_vals = regions[3];
  PhysicalRegion c_vals = regions[4];

  int32_t distFused = task->index_point[0];
  task_2Args* args = (task_2Args*)(task->args);
  int32_t B1_dimension = args->B1_dimension;
  int32_t B2_dimension = args->B2_dimension;
  int32_t a1_dimension = args->a1_dimension;
  int32_t a2_dimension = args->a2_dimension;
  int32_t c1_dimension = args->c1_dimension;
  int32_t gx = args->gx;
  int32_t gy = args->gy;

  auto c_vals_ro_accessor = createAccessor<AccessorROdouble1>(c_vals, FID_VAL);
  auto B_vals_ro_accessor = createAccessor<AccessorROdouble1>(B_vals, FID_VAL);
  auto a_vals_rw_accessor = createAccessor<AccessorRWdouble2>(a_vals, FID_VAL);
  auto B3_pos_accessor = createAccessor<AccessorRORect_1_2>(B3_pos, FID_RECT_1);
  auto B3_crd_accessor = createAccessor<AccessorROint32_t1>(B3_crd, FID_COORD);

  int32_t io = getIndexPoint(task, 0);
  int32_t jo = getIndexPoint(task, 1);
  int64_t pointID2 = io * gy + jo;
  #pragma omp parallel for schedule(static)
  for (int32_t f = ((0 / gx) * ((B2_dimension + (gy - 1)) / gy - 0 / gy) + 0 / gy); f < (((B1_dimension + (gx - 1)) / gx) * ((B2_dimension + (gy - 1)) / gy - 0 / gy) + 0 / gy); f++) {
    int32_t ii = f / ((B2_dimension + (gy - 1)) / gy - 0 / gy);
    int32_t i = io * ((B1_dimension + (gx - 1)) / gx) + ii;
    if (i >= B1_dimension)
      continue;

    if (i >= (io + 1) * ((B1_dimension + (gx - 1)) / gx - 0 / gx))
      continue;

    int32_t ji = f % ((B2_dimension + (gy - 1)) / gy - 0 / gy);
    int32_t j = jo * ((B2_dimension + (gy - 1)) / gy) + ji;
    if (j >= B2_dimension)
      continue;

    if (j >= (jo + 1) * ((B2_dimension + (gy - 1)) / gy - 0 / gy))
      continue;

    for (int32_t kB = B3_pos_accessor[Point<2>(i, j)].lo; kB < (B3_pos_accessor[Point<2>(i, j)].hi + 1); kB++) {
      int32_t k = B3_crd_accessor[kB];
      a_vals_rw_accessor[Point<2>(i, j)] = a_vals_rw_accessor[Point<2>(i, j)] + B_vals_ro_accessor[Point<1>(kB)] * c_vals_ro_accessor[Point<1>(k)];
    }
  }
}

void computeLegionDDS(Context ctx, Runtime* runtime, LegionTensor* a, LegionTensor* B, LegionTensor* c, partitionPackForcomputeLegionDDS* partitionPack, int32_t gx, int32_t gy) {
  int a1_dimension = a->dims[0];
  int a2_dimension = a->dims[1];
  auto a_vals_parent = a->valsParent;
  int B1_dimension = B->dims[0];
  int B2_dimension = B->dims[1];
  auto B3_pos_parent = B->indicesParents[2][0];
  auto B3_crd_parent = B->indicesParents[2][1];
  auto B_vals_parent = B->valsParent;
  int c1_dimension = c->dims[0];
  RegionWrapper c_vals = c->vals;
  auto c_vals_parent = c->valsParent;

  Point<2> lowerBound = Point<2>(0, 0);
  Point<2> upperBound = Point<2>((gx - 1), (gy - 1));
  auto distFusedIndexSpace = runtime->create_index_space(ctx, Rect<2>(lowerBound, upperBound));
  DomainT<2> domain = runtime->get_index_space_domain(ctx, IndexSpaceT<2>(distFusedIndexSpace));
  task_2Args taskArgsRaw;
  taskArgsRaw.B1_dimension = B1_dimension;
  taskArgsRaw.B2_dimension = B2_dimension;
  taskArgsRaw.a1_dimension = a1_dimension;
  taskArgsRaw.a2_dimension = a2_dimension;
  taskArgsRaw.c1_dimension = c1_dimension;
  taskArgsRaw.gx = gx;
  taskArgsRaw.gy = gy;
  TaskArgument taskArgs = TaskArgument(&taskArgsRaw, sizeof(task_2Args));
  IndexLauncher launcher = IndexLauncher(taskID(2), domain, taskArgs, ArgumentMap());
  launcher.add_region_requirement(RegionRequirement(partitionPack->aPartition.valsPartition, 0, READ_WRITE, EXCLUSIVE, a_vals_parent).add_field(FID_VAL));
  launcher.add_region_requirement(RegionRequirement(partitionPack->BPartition.indicesPartitions[2][0], 0, READ_ONLY, EXCLUSIVE, get_logical_region(B3_pos_parent)).add_field(FID_RECT_1));
  launcher.add_region_requirement(RegionRequirement(partitionPack->BPartition.indicesPartitions[2][1], 0, READ_ONLY, EXCLUSIVE, get_logical_region(B3_crd_parent)).add_field(FID_COORD));
  launcher.add_region_requirement(RegionRequirement(partitionPack->BPartition.valsPartition, 0, READ_ONLY, EXCLUSIVE, B_vals_parent).add_field(FID_VAL));
  launcher.add_region_requirement(RegionRequirement(c_vals, READ_ONLY, EXCLUSIVE, c_vals_parent).add_field(FID_VAL));
  runtime->execute_index_space(ctx, launcher);

}
void registerTacoTasks() {
  {
    TaskVariantRegistrar registrar(taskID(1), "task_1");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    registrar.set_leaf();
    Runtime::preregister_task_variant<task_1>(registrar, "task_1");
  }
  {
    TaskVariantRegistrar registrar(taskID(2), "task_2");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    registrar.set_leaf();
    Runtime::preregister_task_variant<task_2>(registrar, "task_2");
  }
}
