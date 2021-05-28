#include "legion.h"
#include "taco_mapper.h"
#include "legion_utils.h"

using namespace Legion;

typedef double valType;

// Defined by the generated TACO code.
void registerTacoTasks();
LogicalPartition placeLegionA(Context ctx, Runtime* runtime, LogicalRegion a, int32_t gridX, int32_t gridY);
LogicalPartition placeLegionB(Context ctx, Runtime* runtime, LogicalRegion b, int32_t gridX, int32_t gridY);
LogicalPartition placeLegionC(Context ctx, Runtime* runtime, LogicalRegion c, int32_t gridX, int32_t gridY);
void computeLegion(Context ctx, Runtime* runtime, LogicalRegion a, LogicalRegion b, LogicalRegion c, int32_t gridX, int32_t gridY);
void top_level_task(const Task* task, const std::vector<PhysicalRegion>& regions, Context ctx, Runtime* runtime) {
  // Create the regions.
  auto args = runtime->get_input_args();
  int n = -1;
  int gx = -1;
  int gy = -1;
  // Parse input args.
  for (int i = 1; i < args.argc; i++) {
    if (strcmp(args.argv[i], "-n") == 0) {
      n = atoi(args.argv[++i]);
      continue;
    }
    if (strcmp(args.argv[i], "-gx") == 0) {
      gx = atoi(args.argv[++i]);
      continue;
    }
    if (strcmp(args.argv[i], "-gy") == 0) {
      gy = atoi(args.argv[++i]);
      continue;
    }
    // TODO (rohany): Add a flag to do the validation or not.
  }
  if (n == -1) {
    std::cout << "Please provide an input matrix size with -n." << std::endl;
    return;
  }
  if (gx == -1) {
    std::cout << "Please provide a grid x size with -gx." << std::endl;
    return;
  }
  if (gy == -1) {
    std::cout << "Please provide a gris y size with -gy." << std::endl;
    return;
  }

  auto fspace = runtime->create_field_space(ctx);
  allocate_tensor_fields<valType>(ctx, runtime, fspace);
  auto ispace = runtime->create_index_space(ctx, Rect<2>({0, 0}, {n - 1, n - 1}));
  auto A = runtime->create_logical_region(ctx, ispace, fspace); runtime->attach_name(A, "A");
  auto B = runtime->create_logical_region(ctx, ispace, fspace); runtime->attach_name(B, "B");
  auto C = runtime->create_logical_region(ctx, ispace, fspace); runtime->attach_name(C, "C");
  tacoFill<valType>(ctx, runtime, A, 0); tacoFill<valType>(ctx, runtime, B, 1); tacoFill<valType>(ctx, runtime, C, 1);

  // Compute on the tensors.
  benchmark(ctx, runtime, [&]() { computeLegion(ctx, runtime, A, B, C, gx, gy); });

  auto a_reg = getRegionToWrite(ctx, runtime, A, A);
  FieldAccessor<READ_WRITE,valType,2,coord_t, Realm::AffineAccessor<valType, 2, coord_t>> a_rw(a_reg, FID_VAL);
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++) {
      assert(a_rw[Point<2>(i, j)] == n);
    }
  }
  runtime->unmap_region(ctx, a_reg);
}

TACO_MAIN(valType)
