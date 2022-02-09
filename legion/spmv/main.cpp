#include "legion.h"
#include "taco_legion_header.h"
#include "hdf5_utils.h"
#include "realm/cmdline.h"
#include "legion_utils.h"
#include "legion_string_utils.h"
#include "error.h"

#ifdef TACO_USE_CUDA
#include "taco-generated.cuh"
#else
#include "taco-generated.h"
#endif

using namespace Legion;
using namespace Legion::Mapping;

typedef double valType;

const int TID_INIT_X = 420;
void initX(const Task* task, const std::vector<PhysicalRegion>& regions, Context ctx, Runtime* runtime) {
  typedef FieldAccessor<WRITE_ONLY,double,1,coord_t,Realm::AffineAccessor<double,1,coord_t>> AccessorD;
  auto x = regions[0];
  auto dom = runtime->get_index_space_domain(x.get_logical_region().get_index_space());
  AccessorD xAcc(x, FID_VAL);
  #pragma omp parallel for schedule(static)
  for (size_t i = dom.lo()[0]; i < size_t(dom.hi()[0]); i++) {
    xAcc[i] = i;
  }
}

void copyRect1(const Task* task, const std::vector<PhysicalRegion>& regions, Context ctx, Runtime* runtime) {
  typedef FieldAccessor<WRITE_ONLY,int32_t,1,coord_t,Realm::AffineAccessor<int32_t,1,coord_t>> AccessorD;
  typedef FieldAccessor<READ_ONLY,Rect<1>,1,coord_t,Realm::AffineAccessor<Rect<1>,1,coord_t>> AccessorR;
  auto x = regions[0];
  auto dom = runtime->get_index_space_domain(x.get_logical_region().get_index_space());
  AccessorD xAcc(x, FID_RECT_1);
  AccessorR yAcc(regions[1], FID_RECT_1);
  // Unpack each Rect<1> into an entry for the current row.
  #pragma omp parallel for schedule(static)
  for (size_t i = dom.lo()[0]; i < size_t(dom.hi()[0]); i++) {
    xAcc[i] = yAcc[i].lo;
  }
  xAcc[dom.hi()[0]] = yAcc[dom.hi()[0] - 1].hi + 1;
}

void convertDISTALCSRToStandardCSR(Context ctx, Runtime* runtime, int pieces, LegionTensor& tensor, partitionPackForcomputeLegionRowSplit& pack, ExternalHDF5LegionTensor& attached) {
  // Create a new region, and collect the old one
  auto fspace = runtime->create_field_space(ctx);
  {
    auto falloc = runtime->create_field_allocator(ctx, fspace);
    falloc.allocate_field(sizeof(int32_t), FID_RECT_1);
  }
  auto ispace = runtime->create_index_space(ctx, Rect<1>(0, tensor.dims[0] + 1));
  auto posCopy = runtime->create_logical_region(ctx, ispace, fspace);
  {
    // TODO (rohany): Expand this to run on multiple node if needed.
    TaskLauncher launcher(TID_COPY_RECT_TO_NORMAL_POS, TaskArgument());
    launcher.add_region_requirement(RegionRequirement(posCopy, WRITE_ONLY, EXCLUSIVE, posCopy).add_field(FID_RECT_1));
    launcher.add_region_requirement(RegionRequirement(tensor.indices[1][0], READ_ONLY, EXCLUSIVE, tensor.indicesParents[1][0]).add_field(FID_RECT_1));
    runtime->execute_task(ctx, launcher).wait();
  }
  auto oldReg = tensor.indicesParents[1][0];
  tensor.indices[1][0] = posCopy;
  tensor.indicesParents[1][0] = posCopy;
  // Create a corresponding partition for the new region.
  DomainPointColoring coloring;
  auto origPart = pack.BPartition.indicesPartitions[1][0];
  for (int i = 0; i < pieces; i++) {
    auto subreg = runtime->get_logical_subregion_by_color(ctx, origPart, i);
    auto bounds = runtime->get_index_space_domain(ctx, subreg.get_index_space());
    coloring[i] = Rect<1>(bounds.lo()[0], bounds.hi()[0] + 1);
  }
  auto ipart = runtime->create_partition_by_domain(ctx, posCopy.get_index_space(), coloring, runtime->get_index_partition_color_space_name(ctx, origPart.get_index_partition()));
  pack.BPartition.indicesPartitions[1][0] = runtime->get_logical_partition(ctx, posCopy, ipart);

  // Delete the old region, as this is important to not OOM on some benchmarks.
  // Before we do so though, detach the region from the external allocation.
  size_t toDelete = 0;
  for (size_t i = 0; i < attached.attachedRegions.size(); i++) {
    auto phys = attached.attachedRegions[i];
    if (phys.get_logical_region() == oldReg) {
      toDelete = i;
      runtime->detach_external_resource(ctx, phys);
    }
  }
  attached.attachedRegions.erase(attached.attachedRegions.begin() + toDelete);
  runtime->destroy_logical_region(ctx, oldReg);
}

// Configuration represents the kind of SpMV benchmark we are running.
enum Configuration {
  CSR_ROW_SPLIT,
  CSR_POS_SPLIT,
  DCSR_POS_SPLIT,
  CSC_SPC,
};

void top_level_task(const Task* task, const std::vector<PhysicalRegion>& regions, Context ctx, Runtime* runtime) {
  std::string csrFileName, dcsrFileName, cscFileName, spxFile;
  bool dump = false, pos = false;
  int n = 10, pieces = 0, warmup = 5;
  Realm::CommandLineParser parser;
  parser.add_option_string("-csr", csrFileName);
  parser.add_option_string("-csc", cscFileName);
  parser.add_option_string("-spx", spxFile);
  parser.add_option_string("-dcsr", dcsrFileName);
  parser.add_option_bool("-dump", dump);
  parser.add_option_int("-n", n);
  parser.add_option_int("-pieces", pieces);
  parser.add_option_int("-warmup", warmup);
  parser.add_option_bool("-pos", pos);
  auto args = Runtime::get_input_args();
  taco_uassert(parser.parse_command_line(args.argc, args.argv)) << "Parse failure.";
  taco_uassert(!csrFileName.empty() || !dcsrFileName.empty() || !cscFileName.empty()) << "Provide a matrix with -csr, -dcsr or -csc.";

  // Figure out how many pieces to chop up the data into.
  if (pieces == 0) {
    pieces = getNumPieces(ctx, runtime);
    taco_uassert(pieces != 0) << "Please provide a number of pieces to split into with -pieces. Unable to automatically find.";
  }

  Configuration conf;
  if (!csrFileName.empty() && !pos) {
    conf = CSR_ROW_SPLIT;
  } else if (!csrFileName.empty() && pos) {
    conf = CSR_POS_SPLIT;
  } else if (!dcsrFileName.empty()) {
    taco_iassert(pos) << "Must use pos split with DCSR matrix";
    conf = DCSR_POS_SPLIT;
  } else {
    taco_iassert(!cscFileName.empty());
    taco_iassert(!spxFile.empty()) << "Must provide sparse X vector file with CSC configuration.";
    conf = CSC_SPC;
  }

  LegionTensor A; ExternalHDF5LegionTensor Aex;
  if (conf == CSR_POS_SPLIT || conf == CSR_ROW_SPLIT) {
    std::tie(A, Aex) = loadLegionTensorFromHDF5File(ctx, runtime, csrFileName, {Dense, Sparse});
  } else if (conf == DCSR_POS_SPLIT) {
    std::tie(A, Aex) = loadLegionTensorFromHDF5File(ctx, runtime, dcsrFileName, {Sparse, Sparse});
  } else {
    taco_iassert(conf == CSC_SPC);
    std::tie(A, Aex) = loadLegionTensorFromHDF5File(ctx, runtime, cscFileName, {Dense, Sparse});
  }

  // Initialize CuSparse if we are running with GPUs.
  initCuSparse(ctx, runtime);
  
  auto eqPartIspace = runtime->create_index_space(ctx, Rect<1>(0, pieces - 1));
  auto eqPartDomain = runtime->get_index_space_domain(eqPartIspace);
  auto y = createDenseTensor<1, valType>(ctx, runtime, {A.dims[0]}, FID_VAL);
  runtime->fill_field(ctx, y.vals, y.valsParent, FID_VAL, valType(0));

  LegionTensor x; ExternalHDF5LegionTensor xEx;
  if (conf == CSC_SPC) {
    std::tie(x, xEx) = loadLegionTensorFromHDF5File(ctx, runtime, spxFile, {Sparse});
  } else {
    x = createDenseTensor<1, valType>(ctx, runtime, {A.dims[1]}, FID_VAL);
    // Initialize x.
    auto xEqPart = runtime->create_equal_partition(ctx, x.vals.get_index_space(), eqPartIspace);
    auto xEqLPart = runtime->get_logical_partition(ctx, x.vals, xEqPart);
    {
      IndexTaskLauncher launcher(TID_INIT_X, eqPartDomain, TaskArgument(), ArgumentMap());
      launcher.add_region_requirement(RegionRequirement(xEqLPart, 0, WRITE_ONLY, EXCLUSIVE, x.valsParent).add_field(FID_VAL));
      runtime->execute_index_space(ctx, launcher);
    }
  }

  // Create a partition of y for forcing reductions.
  auto yEqIndexPart = runtime->create_equal_partition(ctx, y.vals.get_index_space(), eqPartIspace);
  auto yEqLPart = runtime->get_logical_partition(ctx, y.vals, yEqIndexPart);

  // Partition the tensors.
  partitionPackForcomputeLegionPosSplit posPack;
  partitionPackForcomputeLegionRowSplit rowPack;
  partitionPackForcomputeLegionPosSplitDCSR posDCSRPack;
  partitionPackForcomputeLegionCSCMSpV cscPack;
  switch (conf) {
    case CSR_ROW_SPLIT:
      rowPack = partitionForcomputeLegionRowSplit(ctx, runtime, &y, &A, &x, pieces); break;
    case CSR_POS_SPLIT:
      posPack = partitionForcomputeLegionPosSplit(ctx, runtime, &y, &A, &x, pieces); break;
    case DCSR_POS_SPLIT:
      posDCSRPack = partitionForcomputeLegionPosSplitDCSR(ctx, runtime, &y, &A, &x, pieces); break;
    case CSC_SPC:
      cscPack = partitionForcomputeLegionCSCMSpV(ctx, runtime, &y, &A, &x, pieces); break;
  }

#ifdef TACO_USE_CUDA
  // If we are using CUDA and using a row-split schedule, we're going to use CuSparse
  // at the leaves. Therefore, we need to convert the DISTAL pos region into a standard
  // CSR region to be compatible with CuSparse.
  if (conf == CSR_ROW_SPLIT) {
    convertDISTALCSRToStandardCSR(ctx, runtime, pieces, A, rowPack, Aex);
  }
#endif

  // Benchmark the computation.
  auto avgTime = benchmarkAsyncCallWithWarmup(ctx, runtime, warmup, n, [&]() {
    if (dump) { runtime->fill_field(ctx, y.vals, y.valsParent, FID_VAL, valType(0)); }
    switch (conf) {
      case CSR_ROW_SPLIT:
        computeLegionRowSplit(ctx, runtime, &y, &A, &x, &rowPack, pieces); break;
      case CSR_POS_SPLIT:
        computeLegionPosSplit(ctx, runtime, &y, &A, &x, &posPack, pieces);
        launchDummyReadOverPartition(ctx, runtime, y.vals, yEqLPart, FID_VAL, eqPartDomain);
        break;
      case DCSR_POS_SPLIT:
        computeLegionPosSplitDCSR(ctx, runtime, &y, &A, &x, &posDCSRPack, pieces);
        launchDummyReadOverPartition(ctx, runtime, y.vals, yEqLPart, FID_VAL, eqPartDomain);
        break;
      case CSC_SPC:
        computeLegionCSCMSpV(ctx, runtime, &y, &A, &x, &cscPack, pieces);
        launchDummyReadOverPartition(ctx, runtime, y.vals, yEqLPart, FID_VAL, eqPartDomain);
        break;
    }
  });
  LEGION_PRINT_ONCE(runtime, ctx, stdout, "Average execution time: %lf ms\n", avgTime);

  if (dump) {
    printLegionTensor<valType>(ctx, runtime, y);
  }
  Aex.destroy(ctx, runtime);
}

int main(int argc, char** argv) {
  Runtime::set_top_level_task_id(TID_TOP_LEVEL);
  {
    TaskVariantRegistrar registrar(TID_TOP_LEVEL, "top_level");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    registrar.set_replicable();
    Runtime::preregister_task_variant<top_level_task>(registrar, "top_level");
  }
  if (TACO_FEATURE_OPENMP) {
    TaskVariantRegistrar registrar(TID_TOP_LEVEL, "top_level");
    registrar.add_constraint(ProcessorConstraint(Processor::OMP_PROC));
    registrar.set_replicable();
    Runtime::preregister_task_variant<top_level_task>(registrar, "top_level");
  }
  {
    TaskVariantRegistrar registrar(TID_INIT_X, "initXCPU");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    Runtime::preregister_task_variant<initX>(registrar, "initX");
  }
  {
    TaskVariantRegistrar registrar(TID_INIT_X, "initXOMP");
    registrar.add_constraint(ProcessorConstraint(Processor::OMP_PROC));
    Runtime::preregister_task_variant<initX>(registrar, "initX");
  }
  {
    TaskVariantRegistrar registrar(TID_COPY_RECT_TO_NORMAL_POS, "copyrect");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    Runtime::preregister_task_variant<copyRect1>(registrar, "copyRect");
  }
  {
    TaskVariantRegistrar registrar(TID_COPY_RECT_TO_NORMAL_POS, "copyrect");
    registrar.add_constraint(ProcessorConstraint(Processor::OMP_PROC));
    Runtime::preregister_task_variant<copyRect1>(registrar, "copyRect");
  }
  registerHDF5UtilTasks();
  registerTacoTasks();
  registerDummyReadTasks();
  registerTacoRuntimeLibTasks();
  initCuSparseAtStartup();
  Runtime::add_registration_callback(register_taco_mapper);
  Runtime::preregister_sharding_functor(TACOShardingFunctorID, new TACOShardingFunctor());
  return Runtime::start(argc, argv);
}
