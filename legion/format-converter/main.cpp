#include "legion.h"
#include "taco_legion_header.h"
#include "legion_string_utils.h"
#include "hdf5_utils.h"
#include "realm/cmdline.h"

using namespace Legion;

enum TaskIDs {
  TID_TOP_LEVEL,
};

// Forward declarations of all generated conversion methods.
void registerTacoTasks();
void packLegionCOOToCSR(Context ctx, Runtime* runtime, LegionTensor* T, LegionTensor* TCOO);
void packLegionCOOToDSS(Context ctx, Runtime* runtime, LegionTensor* T, LegionTensor* TCOO);
void packLegionCOOToDDS(Context ctx, Runtime* runtime, LegionTensor* T, LegionTensor* TCOO);
// End declarations.

Realm::Logger logApp("app");

void top_level_task(const Task* task, const std::vector<PhysicalRegion>& regions, Context ctx, Runtime* runtime) {
  bool roundTrip = false, dump = false;
  std::string outputFormat, cooFile, outputFile;
  Realm::CommandLineParser parser;
  parser.add_option_string("-coofile", cooFile);
  parser.add_option_string("-format", outputFormat);
  parser.add_option_string("-o", outputFile);
  parser.add_option_bool("-roundTrip", roundTrip);
  parser.add_option_bool("-dump", dump);
  auto args = Runtime::get_input_args();
  assert(parser.parse_command_line(args.argc, args.argv));
  assert(!cooFile.empty());
  assert(!outputFormat.empty());
  assert(!outputFile.empty());

  logApp.info() << "Loading input COO tensor.";
  // Load the input COO tensor.
  auto coo = loadCOOFromHDF5(ctx, runtime, cooFile, FID_RECT_1, FID_COORD, sizeof(int32_t), FID_VAL, sizeof(double));
  logApp.info() << "Done loading input COO tensor.";

  // Construct the desired output format.
  std::vector<LegionTensorLevelFormat> format;
  for (auto c : outputFormat) {
    switch (c) {
      case 'd':
        format.push_back(Dense); break;
      case 's':
        format.push_back(Sparse); break;
      default:
        assert(false && "invalid sparse input");
    }
  }
  assert(int(format.size()) == coo.order);

  // Create the output tensor.
  auto output = createSparseTensorForPack<double>(ctx, runtime, format, coo.dims, FID_RECT_1, FID_COORD, FID_VAL);

  logApp.info() << "Packing COO to desired format.";

  // TODO (rohany): Is there any way that we could generate this map programmatically? We could
  //  probably emit it at the top of the taco generated file?
  typedef void (*ConvFunc)(Context, Runtime*, LegionTensor*, LegionTensor*);
  std::map<std::string, ConvFunc> converters = {
      {"ds", packLegionCOOToCSR},
      {"dss", packLegionCOOToDSS},
      {"dds", packLegionCOOToDDS},
  };
  auto it = converters.find(outputFormat);
  if (it == converters.end()) {
    assert(false && "unsupported output format kind");
  }
  it->second(ctx, runtime, &output, &coo);

  logApp.info() << "Done packing COO to desired format.";

  logApp.info() << "Dumping output tensor to HDF5 file.";
  // Now, dump the output tensor to an HDF5 file.
  dumpLegionTensorToHDF5File(ctx, runtime, output, format, outputFile);
  logApp.info() << "Done!";

  if (roundTrip) {
    // Let's try and load it back in to see if it somewhat round-trips.
    auto test = loadLegionTensorFromHDF5File(ctx, runtime, outputFile, format);
    logApp.info() << test.toString(ctx, runtime);
  }
  if (dump) {
    // Dump out the packed tensor to stdout.
    printLegionTensor<double>(ctx, runtime, output, format);
  }
}

int main(int argc, char** argv) {
  Runtime::set_top_level_task_id(TID_TOP_LEVEL);
  {
    TaskVariantRegistrar registrar(TID_TOP_LEVEL, "top_level");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    registrar.set_replicable();
    Runtime::preregister_task_variant<top_level_task>(registrar, "top_level");
  }
  registerHDF5UtilTasks();
  registerTacoTasks();
  return Runtime::start(argc, argv);
}
