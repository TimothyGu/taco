#include "legion.h"
#include "realm/cmdline.h"
#include "strings.h"
#include "hdf5_utils.h"
#include <fstream>
#include <iomanip>

using namespace Legion;

/*
 * This tool is used for converting .tns files into HDF5 files that can easily
 * be loaded into Legion through attach operations. This tool as two uses:
 *   ./bin/tnstohdf5 -tns f1 -hdf5 f2 // dump f1 into f2 as tns
 *   ./bin/tnstohdf5 -hdf5 f -dump // dump the contents of f
 * The tool dumps the tns into an HDF5 file with the following format:
 *              [coord0 coord1 coord2 ... coordN value]
 * where each coord0 is an int32_t, and value is a double. There are nnz
 * entries in the resulting data structure.
 */

enum TaskIDs {
  TID_TOP_LEVEL,
  TID_PRINT_DATA,
};

enum FieldIDs {
  FID_DIM,
  FID_VALUE,
  FID_COORD, // This must be the last defined field.
};

static size_t numIntsToCompare = 0;
static int lexComp(const void* a, const void* b) {
  for (size_t i = 0; numIntsToCompare; i++) {
    int diff = ((int32_t*)a)[i] - ((int32_t*)b)[i];
    if (diff != 0) {
      return diff;
    }
  }
  return 0;
}

void* readMTXFile(std::string fileName, std::vector<int32_t>& dimensions, size_t& nnz) {
  // fileName must be a relative, sanitized path.
  std::fstream file;
  file.open(fileName, std::fstream::in);

  // TODO (rohany): Parse the header.

  std::string line;
  std::getline(file, line);

  // Skip comments at the top of the file
  std::string token;
  do {
    std::stringstream lineStream(line);
    lineStream >> token;
    if (token[0] != '%') {
      break;
    }
  } while (std::getline(file, line));

  size_t order = 2;
  
  char* linePtr = (char*)line.data();
  while (size_t dimension = strtoul(linePtr, &linePtr, 10)) {
    // taco_uassert(dimension <= INT_MAX) << "Dimension exceeds INT_MAX";
    dimensions.push_back(static_cast<int>(dimension));
  }
  nnz = dimensions[dimensions.size()-1];
  dimensions.pop_back();

  // The coordinates need to be sorted, otherwise we'll have to deal with the headache
  // of sorting them in legion. In order to do this (and keep the values along with the
  // coordinates) without doing a bunch of allocations, we have to drop into lower level code.
  // To do this, we'll allocate a flat buffer of "structs" where each struct has size
  // coords * coord_size + val_size. We can write individual elements into this buffer.
  // Then, we'll do a standard doubling array to grow the buffer as we read in elements.
  // This flat representation will let us then directly call qsort on the data like TACO.
  // If this becomes a bottleneck and we need to utilize a parallel sort, we can try and
  // implement the operation described here:
  // https://stackoverflow.com/questions/16874183/reusing-stdalgorithms-with-non-standard-containers/16905832

  // TODO (rohany): Dedup this into a class.

  // Set up the constants and buffers.
  size_t elemSize = order * sizeof(int32_t) + sizeof(double);
  size_t cnt = 0;
  // TODO (rohany): Adjust this initial size when loading larger files. It might
  //  even be a good idea to take this in via a command line parameter to have it
  //  fit to the correct size if we know it apriori.
  size_t size = 1;
  char* buffer = (char*)malloc(elemSize * size);

  // Load data from the tns file.
  while (std::getline(file, line)) {
    // If we've hit the buffer capacity, then allocate a fresh buffer.
    if (cnt == size) {
      size *= 2;
      buffer = (char*)realloc(buffer, elemSize * size);
    }
    // Get the front of the buffer where we should insert into.
    char* insert = (elemSize * cnt) + buffer;
    char* linePtr = (char*)line.data();
    for (size_t i = 0; i < order; i++) {
      int32_t idx = strtol(linePtr, &linePtr, 10);
      assert(idx <= INT_MAX && "Coordinate in file is larger than INT_MAX");
      // .tns coordinates 1 indexed rather than 0 indexed.
      *(int32_t*)insert = (idx - 1);
      // The largest value in each dimension is the size of the dimension.
      dimensions[i] = std::max(dimensions[i], idx);
      // Advance the pointer one int32_t position for the next coordinate.
      insert += sizeof(int32_t);
    }
    // After all of the int32_t coordinates, the last position remaining
    // is for the double.
    // double val = strtod(linePtr, &linePtr);
    double val = 1.0;
    *(double*)insert = val;
    cnt++;
  }

  // Clean up.
  file.close();

  // Now, let's sort the coordinates before dumping them. We'll use the qsort
  // function with a custom comparator.
  numIntsToCompare = order;
  qsort(buffer, cnt, elemSize, lexComp);

  nnz = cnt;
  return buffer;
}

// This function performs a direct translation from a tns file into an HDF5 version
// of the tns file for easier interop with Legion later. It returns flat buffer that
// is an AOS representation of the coordinates and the value for each entry. The result
// buffer must be free'd by the user.
void* readTNSFile(std::string fileName, std::vector<int32_t>& dimensions, size_t& nnz) {
  // fileName must be a relative, sanitized path.
  std::fstream file;
  file.open(fileName, std::fstream::in);

  std::string line;
  if (!std::getline(file, line)) {
    std::cout << "Expected non-empty tns file." << std::endl;
    assert(false);
  }

  std::vector<std::string> toks = split(line, " ", false /* keepDelim */);
  size_t order = toks.size() - 1;
  dimensions = std::vector<int32_t>(order);

  // The coordinates need to be sorted, otherwise we'll have to deal with the headache
  // of sorting them in legion. In order to do this (and keep the values along with the
  // coordinates) without doing a bunch of allocations, we have to drop into lower level code.
  // To do this, we'll allocate a flat buffer of "structs" where each struct has size
  // coords * coord_size + val_size. We can write individual elements into this buffer.
  // Then, we'll do a standard doubling array to grow the buffer as we read in elements.
  // This flat representation will let us then directly call qsort on the data like TACO.
  // If this becomes a bottleneck and we need to utilize a parallel sort, we can try and
  // implement the operation described here:
  // https://stackoverflow.com/questions/16874183/reusing-stdalgorithms-with-non-standard-containers/16905832

  // Set up the constants and buffers.
  size_t elemSize = order * sizeof(int32_t) + sizeof(double);
  size_t cnt = 0;
  // TODO (rohany): Adjust this initial size when loading larger files. It might
  //  even be a good idea to take this in via a command line parameter to have it
  //  fit to the correct size if we know it apriori.
  size_t size = 1;
  char* buffer = (char*)malloc(elemSize * size);

  // Load data from the tns file.
  do {
    // If we've hit the buffer capacity, then allocate a fresh buffer.
    if (cnt == size) {
      size *= 2;
      buffer = (char*)realloc(buffer, elemSize * size);
    }
    // Get the front of the buffer where we should insert into.
    char* insert = (elemSize * cnt) + buffer;
    char* linePtr = (char*)line.data();
    for (size_t i = 0; i < order; i++) {
      int32_t idx = strtol(linePtr, &linePtr, 10);
      assert(idx <= INT_MAX && "Coordinate in file is larger than INT_MAX");
      // .tns coordinates 1 indexed rather than 0 indexed.
      *(int32_t*)insert = (idx - 1);
      // The largest value in each dimension is the size of the dimension.
      dimensions[i] = std::max(dimensions[i], idx);
      // Advance the pointer one int32_t position for the next coordinate.
      insert += sizeof(int32_t);
    }
    // After all of the int32_t coordinates, the last position remaining
    // is for the double.
    double val = strtod(linePtr, &linePtr);
    *(double*)insert = val;
    cnt++;
  } while (std::getline(file, line));

  // Clean up.
  file.close();

  // Now, let's sort the coordinates before dumping them. We'll use the qsort
  // function with a custom comparator.
  numIntsToCompare = order;
  qsort(buffer, cnt, elemSize, lexComp);

  nnz = cnt;
  return buffer;
}

void readHDF5Coords(const Task* task, const std::vector<PhysicalRegion> &regions, Context ctx, Runtime* runtime) {
  auto region = regions[0];

  // Declare accessors.
  typedef FieldAccessor<READ_ONLY,int32_t,1,coord_t, Realm::AffineAccessor<int32_t, 1, coord_t>> AccessorI;
  typedef FieldAccessor<READ_ONLY,double,1,coord_t, Realm::AffineAccessor<double, 1, coord_t>> AccessorD;

  std::vector<FieldID> fields;
  region.get_fields(fields);
  size_t order = fields.size() - 1;

  // Create an accessor for the dimensions.
  AccessorI dimsAcc(regions[1], FID_DIM);
  std::cout << "Dims: " << std::endl;
  for (size_t i = 0; i < order; i++) {
    std::cout << dimsAcc[i] << " ";
  }
  std::cout << std::endl;

  // Create an accessor for each of the fields corresponding to coordinates.
  std::vector<AccessorI> coords;
  for (size_t i = 0; i < order; i++) {
    coords.push_back(AccessorI(region, FID_COORD + i));
  }

  // Create an accessor for the values.
  AccessorD vals(region, FID_VALUE);

  for (PointInRectIterator<1> itr(region); itr(); itr++) {
    for (auto acc : coords) {
      std::cout << acc[*itr] << " ";
    }
    std::cout << std::setprecision(9) << vals[*itr] << std::endl;
  }
}

std::vector<FieldID> fieldIDs(int order) {
  std::vector<FieldID> result;
  for (int i = 0; i < order; i++) {
    result.push_back(FID_COORD + i);
  }
  result.push_back(FID_VALUE);
  return result;
}

void allocateCoordFields(Context ctx, Runtime* runtime, std::vector<FieldID> fields, FieldSpace f) {
  Legion::FieldAllocator allocator = runtime->create_field_allocator(ctx, f);
  for (size_t i = 0; i < fields.size(); i++) {
    // Assume that the fields are laid out as [coord0, coord1, ..., coordN, value].
    if (i == fields.size() - 1) {
      allocator.allocate_field(sizeof(double), fields[i]);
    } else {
      allocator.allocate_field(sizeof(int32_t), fields[i]);
    }
  }
}

std::map<FieldID, const char*> constructFieldMap(std::vector<FieldID> fields) {
  std::map<FieldID, const char*> result;
  for (size_t i = 0; i < fields.size(); i++) {
    if (i == fields.size() - 1) {
      result[fields[i]] = COOValsField;
    } else {
      result[fields[i]] = COOCoordsFields[i];
    }
  }
  return result;
}

struct ProgramRegions {
  LogicalRegion mem;
  LogicalRegion disk;
  LogicalRegion memDim;
  LogicalRegion diskDim;
};
ProgramRegions createRegions(Context ctx, Runtime* runtime, size_t order, size_t nnz) {
  ProgramRegions result;
  // Create regions for the coord/vals data.
  {
    auto fspace = runtime->create_field_space(ctx);
    auto coordFieldIDs = fieldIDs(order);
    allocateCoordFields(ctx, runtime, coordFieldIDs, fspace);
    auto ispace = runtime->create_index_space(ctx, Rect<1>(0, nnz - 1));
    result.mem = runtime->create_logical_region(ctx, ispace, fspace);
    result.disk = runtime->create_logical_region(ctx, ispace, fspace);
  }
  // Create regions for the dimension data.
  {
    auto fspace = runtime->create_field_space(ctx);
    Legion::FieldAllocator alloc = runtime->create_field_allocator(ctx, fspace);
    alloc.allocate_field(sizeof(int32_t), FID_DIM);
    auto ispace = runtime->create_index_space(ctx, Rect<1>(0, order - 1));
    result.memDim = runtime->create_logical_region(ctx, ispace, fspace);
    result.diskDim = runtime->create_logical_region(ctx, ispace, fspace);
  }
  return result;
}

void top_level_task(const Task* task, const std::vector<PhysicalRegion>&, Context ctx, Runtime* runtime) {
  std::string tnsFilename;
  std::string hdf5Filename;
  bool dumpOnly = false;

  Realm::CommandLineParser parser;
  parser.add_option_string("-tns", tnsFilename);
  parser.add_option_string("-hdf5", hdf5Filename);
  parser.add_option_bool("-dump", dumpOnly);
  auto args = Runtime::get_input_args();
  assert(parser.parse_command_line(args.argc, args.argv));
  assert(!hdf5Filename.empty());

  if (dumpOnly) {
    size_t order, nnz;
    getCoordListHDF5Meta(hdf5Filename, order, nnz);
    auto regions = createRegions(ctx, runtime, order, nnz);
    auto disk = regions.disk;
    auto diskDim = regions.diskDim;

    // Attach the regions.
    auto coordFieldIDs = fieldIDs(order);
    auto fieldMap = constructFieldMap(coordFieldIDs);
    auto pdisk = attachHDF5(ctx, runtime, disk, fieldMap, hdf5Filename);
    auto pdiskDim = attachHDF5(ctx, runtime, diskDim, {{FID_DIM, COODimsField}}, hdf5Filename);

    // Launch a task to print out the result. This maps the region
    // into CPU memory for us to access directly.
    TaskLauncher launcher(TID_PRINT_DATA, TaskArgument());
    RegionRequirement req(disk, READ_ONLY, EXCLUSIVE, disk);
    for (size_t i = 0; i < order; i++) {
      req.add_field(coordFieldIDs[i]);
    }
    req.add_field(FID_VALUE);
    launcher.add_region_requirement(req);
    RegionRequirement req2(diskDim, READ_ONLY, EXCLUSIVE, diskDim);
    req2.add_field(FID_DIM);
    launcher.add_region_requirement(req2);
    runtime->execute_task(ctx, launcher).wait();

    // Clean up.
    runtime->detach_external_resource(ctx, pdisk).wait();
    runtime->detach_external_resource(ctx, pdiskDim).wait();
    return;
  }

  // At this point, the tns filename must be defined.
  assert(!tnsFilename.empty());

  // Read in the .tns file into raw data in memory.
  std::vector<int32_t> dimensions;
  size_t nnz;
  // auto buf = readTNSFile(tnsFilename, dimensions, nnz);
  auto buf = readMTXFile(tnsFilename, dimensions, nnz);
  size_t order = dimensions.size();

  auto regions = createRegions(ctx, runtime, order, nnz);
  auto mem = regions.mem;
  auto disk = regions.disk;
  auto memDim = regions.memDim;
  auto diskDim = regions.diskDim;
  auto coordFieldIDs = fieldIDs(order);

  PhysicalRegion pmem;
  // Get the local CPU memory.
  Memory sysmem = Machine::MemoryQuery(Machine::get_machine())
      .has_affinity_to(runtime->get_executing_processor(ctx))
      .only_kind(Memory::SYSTEM_MEM)
      .first();
  // Attach the in-memory data to the memory region.
  {
    AttachLauncher al(LEGION_EXTERNAL_INSTANCE, mem, mem);
    al.attach_array_aos(buf, false /* column_major */, coordFieldIDs, sysmem);
    pmem = runtime->attach_external_resource(ctx, al);
  }

  // Create the HDF5 file.
  generateCoordListHDF5(hdf5Filename, order, nnz);

  // Now, open up and attach the output hdf5 file.
  auto fieldMap = constructFieldMap(coordFieldIDs);
  auto pdisk = attachHDF5(ctx, runtime, disk, fieldMap, hdf5Filename, LEGION_FILE_READ_WRITE);

  // Finally, copy the in-memory instance into the disk instance.
  {
    CopyLauncher cl;
    cl.add_copy_requirements(
        RegionRequirement(mem, READ_ONLY, EXCLUSIVE, mem),
        RegionRequirement(disk, WRITE_DISCARD, EXCLUSIVE, disk)
    );
    // Copy each of the coordinate fields and the value.
    for (size_t i = 0 ; i < order; i++) {
      cl.add_src_field(0, coordFieldIDs[i]);
      cl.add_dst_field(0, coordFieldIDs[i]);
    }
    cl.add_src_field(0, FID_VALUE);
    cl.add_dst_field(0, FID_VALUE);
    runtime->issue_copy_operation(ctx, cl);
  }

  // Detach the external resources to flush any changes made.
  runtime->detach_external_resource(ctx, pmem).wait();
  runtime->detach_external_resource(ctx, pdisk).wait();

  // Free the buffer holding the data.
  free(buf);

  // Now, do the same for the dimension data.
  PhysicalRegion pmemDim;
  {
    AttachLauncher al(LEGION_EXTERNAL_INSTANCE, memDim, memDim);
    al.attach_array_aos(dimensions.data(), false /* column_major */, {FID_DIM}, sysmem);
    pmemDim = runtime->attach_external_resource(ctx, al);
  }
  auto pdiskDim = attachHDF5(ctx, runtime, diskDim, {{FID_DIM, COODimsField}}, hdf5Filename, LEGION_FILE_READ_WRITE);
  {
    CopyLauncher cl;
    cl.add_copy_requirements(
      RegionRequirement(memDim, READ_ONLY, EXCLUSIVE, memDim),
      RegionRequirement(diskDim, WRITE_DISCARD, EXCLUSIVE, diskDim)
    );
    cl.add_src_field(0, FID_DIM);
    cl.add_dst_field(0, FID_DIM);
    runtime->issue_copy_operation(ctx, cl);
  }
  runtime->detach_external_resource(ctx, pmemDim).wait();
  runtime->detach_external_resource(ctx, pdiskDim).wait();
}

int main(int argc, char** argv) {
  Runtime::set_top_level_task_id(TID_TOP_LEVEL);
  {
    TaskVariantRegistrar registrar(TID_TOP_LEVEL, "top_level");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    Runtime::preregister_task_variant<top_level_task>(registrar, "top_level");
  }
  {
    TaskVariantRegistrar registrar(TID_PRINT_DATA, "printData");
    registrar.add_constraint(ProcessorConstraint(Processor::LOC_PROC));
    Runtime::preregister_task_variant<readHDF5Coords>(registrar, "printData");
  }
  return Runtime::start(argc, argv);
}