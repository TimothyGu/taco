#include "taco_mapper.h"
#include "mappers/logging_wrapper.h"
#include "error.h"
#include "realm/logging.h"
#include "legion_tensor.h"
#include "mapping_utilities.h"

using namespace Legion;
using namespace Legion::Mapping;

const char* TACOMapperName = "TACOMapper";

Realm::Logger logTacoMapper("tacoMapper");

void register_taco_mapper(Machine machine, Runtime *runtime, const std::set<Processor> &local_procs) {
  // If we're supposed to backpressure task executions, then we need to only
  // have a single mapper per node. Otherwise, we can use a mapper per processor.
  bool backpressure = false, oneMapperPerNode = false;
  auto args = Legion::Runtime::get_input_args();
  for (int i = 1; i < args.argc; i++) {
    if (strcmp(args.argv[i], "-tm:enable_backpressure") == 0) {
      backpressure = true;
      break;
    }
    if (strcmp(args.argv[i], "-tm:one_mapper_per_node") == 0) {
      oneMapperPerNode = true;
    }
  }

  if (backpressure || oneMapperPerNode) {
    auto proc = *local_procs.begin();
#ifdef TACO_USE_LOGGING_MAPPER
    runtime->replace_default_mapper(new Mapping::LoggingWrapper(new TACOMapper(runtime->get_mapper_runtime(), machine, proc, TACOMapperName)), Processor::NO_PROC);
#else
    runtime->replace_default_mapper(new TACOMapper(runtime->get_mapper_runtime(), machine, proc, TACOMapperName), Processor::NO_PROC);
#endif
  } else {
    for (auto it : local_procs) {
#ifdef TACO_USE_LOGGING_MAPPER
      runtime->replace_default_mapper(new Mapping::LoggingWrapper(new TACOMapper(runtime->get_mapper_runtime(), machine, it, TACOMapperName)), it);
#else
      runtime->replace_default_mapper(new TACOMapper(runtime->get_mapper_runtime(), machine, it, TACOMapperName), it);
#endif
    }
  }
}

TACOMapper::TACOMapper(Legion::Mapping::MapperRuntime *rt, Legion::Machine &machine, const Legion::Processor &local, const char* name)
    : DefaultMapper(rt, machine, local, name) {
  {
    int argc = Legion::HighLevelRuntime::get_input_args().argc;
    char **argv = Legion::HighLevelRuntime::get_input_args().argv;
    for (int i = 1; i < argc; i++) {
#define BOOL_ARG(argname, varname) do {       \
          if (!strcmp(argv[i], (argname))) {  \
            varname = true;                   \
            continue;                         \
          } } while(0);
#define INT_ARG(argname, varname) do {      \
          if (!strcmp(argv[i], (argname))) {  \
            varname = atoi(argv[++i]);      \
            continue;                       \
          } } while(0);
      BOOL_ARG("-tm:fill_cpu", this->preferCPUFill);
      BOOL_ARG("-tm:validate_cpu", this->preferCPUValidate);
      BOOL_ARG("-tm:untrack_valid_regions", this->untrackValidRegions);
      BOOL_ARG("-tm:numa_aware_alloc", this->numaAwareAllocs);
      BOOL_ARG("-tm:enable_backpressure", this->enableBackpressure);
      INT_ARG("-tm:backpressure_max_in_flight", this->maxInFlightTasks);
      BOOL_ARG("-tm:multiple_shards_per_node", this->multipleShardsPerNode);
      BOOL_ARG("-tm:align128", this->alignTo128Bytes);
#undef BOOL_ARG
    }
  }

  // Record for each OpenMP processor what NUMA region is the closest.
  for (auto proc : this->local_omps) {
    Machine::MemoryQuery local(this->machine);
    local.local_address_space()
         .only_kind(Memory::SOCKET_MEM)
         .best_affinity_to(proc)
         ;
    if (local.count() > 0) {
      this->numaDomains[proc] = local.first();
    }
  }

  if (this->multipleShardsPerNode) {
    // If we have GPUs, map each local CPU to a local GPU corresponding to each shard.
    // If we actually have GPUs, then the number of GPUs should be equal to the number of CPUs.
    // Note that this makes the most sense to do in map_replicate_task, but that is only executed
    // on a single mapper! Therefore, we need to do this in initialization so that each mapper has
    // this data structure populated.
    if (this->local_gpus.size() > 0) {
      taco_iassert(this->local_cpus.size() == this->local_gpus.size());
      auto cpu = this->local_cpus.begin();
      auto gpu = this->local_gpus.begin();
      // Construct a mapping between each shard and a GPU.
      for (; cpu != this->local_cpus.end() && gpu != this->local_gpus.end(); cpu++,gpu++) {
        this->shardCPUGPUMapping[*cpu] = *gpu;
      }
    }
  }

  {
    Machine::MemoryQuery localSysMems(this->machine);
    localSysMems.local_address_space().only_kind(Memory::SYSTEM_MEM);
    Machine::MemoryQuery localSocketMems(this->machine);
    localSocketMems.local_address_space().only_kind(Memory::SOCKET_MEM);
    // Right now, we prefer socket memories to system memory, and will generally use either
    // a single CPU memory, or multiple socket memories. So, take the maximum as the number
    // of local memories to use.
    this->localCPUMems = std::max(localSysMems.count(), localSocketMems.count());
  }
}

void TACOMapper::select_sharding_functor(const Legion::Mapping::MapperContext ctx,
                                         const Legion::Task &task,
                                         const SelectShardingFunctorInput &input,
                                         SelectShardingFunctorOutput &output) {
  // See if there is something special that we need to do. Otherwise, return
  // the TACO sharding functor.
  if ((task.tag & PLACEMENT_SHARD) != 0) {
    int *args = (int *) (task.args);
    // TODO (rohany): This logic makes it look like an argument
    //  serializer / deserializer like is done in Legate would be helpful.
    // The shard ID is the first argument. The generated code registers the desired
    // sharding functor before launching the task.
    Legion::ShardingID shardingID = args[0];
    output.chosen_functor = shardingID;
  } else {
    output.chosen_functor = TACOShardingFunctorID;
  }
}

void TACOMapper::select_task_options(const Legion::Mapping::MapperContext ctx,
                                     const Legion::Task &task,
                                     TaskOptions &output) {
  DefaultMapper::select_task_options(ctx, task, output);
  // Override the default options if we are supposed to run multiple
  // shards per node.
  if (this->multipleShardsPerNode && task.get_depth() == 0) {
    output.replicate = true;
  }
}

void TACOMapper::map_task_impl(const Legion::Mapping::MapperContext ctx, const Legion::Task &task,
                               const MapTaskInput &input, MapTaskOutput &output) {
  Processor::Kind target_kind = task.target_proc.kind();
  // Get the variant that we are going to use to map this task
  VariantInfo chosen = default_find_preferred_variant(task, ctx,
                                                      true/*needs tight bound*/, true/*cache*/, target_kind);
  output.chosen_variant = chosen.variant;
  output.task_priority = default_policy_select_task_priority(ctx, task);
  output.postmap_task = false;
  // Figure out our target processors
  default_policy_select_target_processors(ctx, task, output.target_procs);
  Processor target_proc = output.target_procs[0];
  // See if we have an inner variant, if we do virtually map all the regions
  // We don't even both caching these since they are so simple
  if (chosen.is_inner) {
    // Check to see if we have any relaxed coherence modes in which
    // case we can no longer do virtual mappings so we'll fall through
    bool has_relaxed_coherence = false;
    for (unsigned idx = 0; idx < task.regions.size(); idx++) {
      if (task.regions[idx].prop != LEGION_EXCLUSIVE) {
        has_relaxed_coherence = true;
        break;
      }
    }
    if (!has_relaxed_coherence) {
      std::vector<unsigned> reduction_indexes;
      for (unsigned idx = 0; idx < task.regions.size(); idx++) {
        // As long as this isn't a reduction-only region requirement
        // we will do a virtual mapping, for reduction-only instances
        // we will actually make a physical instance because the runtime
        // doesn't allow virtual mappings for reduction-only privileges
        if (task.regions[idx].privilege == LEGION_REDUCE)
          reduction_indexes.push_back(idx);
        else
          output.chosen_instances[idx].push_back(
              PhysicalInstance::get_virtual_instance());
      }
      if (!reduction_indexes.empty()) {
        const TaskLayoutConstraintSet &layout_constraints =
            runtime->find_task_layout_constraints(ctx,
                                                  task.task_id, output.chosen_variant);
        for (std::vector<unsigned>::const_iterator it =
            reduction_indexes.begin(); it !=
                                       reduction_indexes.end(); it++) {
          MemoryConstraint mem_constraint =
              find_memory_constraint(ctx, task, output.chosen_variant, *it);
          Memory target_memory = default_policy_select_target_memory(ctx,
                                                                     target_proc,
                                                                     task.regions[*it],
                                                                     mem_constraint);
          std::set<FieldID> copy = task.regions[*it].privilege_fields;
          size_t footprint;
          if (!default_create_custom_instances(ctx, target_proc,
                                               target_memory, task.regions[*it], *it, copy,
                                               layout_constraints, false/*needs constraint check*/,
                                               output.chosen_instances[*it], &footprint)) {
            default_report_failed_instance_creation(task, *it,
                                                    target_proc, target_memory, footprint);
          }
        }
      }
      return;
    }
  }
  // Should we cache this task?
  CachedMappingPolicy cache_policy =
      default_policy_select_task_cache_policy(ctx, task);

  // First, let's see if we've cached a result of this task mapping
  const unsigned long long task_hash = compute_task_hash(task);
  std::pair<TaskID, Processor> cache_key(task.task_id, target_proc);
  std::map<std::pair<TaskID, Processor>,
      std::list<CachedTaskMapping> >::const_iterator
      finder = cached_task_mappings.find(cache_key);
  // This flag says whether we need to recheck the field constraints,
  // possibly because a new field was allocated in a region, so our old
  // cached physical instance(s) is(are) no longer valid
  bool needs_field_constraint_check = false;
  if (cache_policy == DEFAULT_CACHE_POLICY_ENABLE && finder != cached_task_mappings.end()) {
    bool found = false;
    // Iterate through and see if we can find one with our variant and hash
    for (std::list<CachedTaskMapping>::const_iterator it =
        finder->second.begin(); it != finder->second.end(); it++) {
      if ((it->variant == output.chosen_variant) &&
          (it->task_hash == task_hash)) {
        // Have to copy it before we do the external call which
        // might invalidate our iterator
        output.chosen_instances = it->mapping;
        output.output_targets = it->output_targets;
        output.output_constraints = it->output_constraints;
        found = true;
        break;
      }
    }
    if (found) {
      // See if we can acquire these instances still
      if (runtime->acquire_and_filter_instances(ctx,
                                                output.chosen_instances))
        return;
      // We need to check the constraints here because we had a
      // prior mapping and it failed, which may be the result
      // of a change in the allocated fields of a field space
      needs_field_constraint_check = true;
      // If some of them were deleted, go back and remove this entry
      // Have to renew our iterators since they might have been
      // invalidated during the 'acquire_and_filter_instances' call
      default_remove_cached_task(ctx, output.chosen_variant,
                                 task_hash, cache_key, output.chosen_instances);
    }
  }
  // We didn't find a cached version of the mapping so we need to
  // do a full mapping, we already know what variant we want to use
  // so let's use one of the acceleration functions to figure out
  // which instances still need to be mapped.
  std::vector<std::set<FieldID> > missing_fields(task.regions.size());
  runtime->filter_instances(ctx, task, output.chosen_variant,
                            output.chosen_instances, missing_fields);
  // Track which regions have already been mapped
  std::vector<bool> done_regions(task.regions.size(), false);
  if (!input.premapped_regions.empty())
    for (std::vector<unsigned>::const_iterator it =
        input.premapped_regions.begin(); it !=
                                         input.premapped_regions.end(); it++)
      done_regions[*it] = true;
  const TaskLayoutConstraintSet &layout_constraints =
      runtime->find_task_layout_constraints(ctx,
                                            task.task_id, output.chosen_variant);
  // Now we need to go through and make instances for any of our
  // regions which do not have space for certain fields
  for (unsigned idx = 0; idx < task.regions.size(); idx++) {
    if (done_regions[idx])
      continue;
    // Skip any empty regions
    if ((task.regions[idx].privilege == LEGION_NO_ACCESS) ||
        (task.regions[idx].privilege_fields.empty()) ||
        missing_fields[idx].empty())
      continue;
    // See if this is a reduction
    MemoryConstraint mem_constraint =
        find_memory_constraint(ctx, task, output.chosen_variant, idx);
    Memory target_memory = default_policy_select_target_memory(ctx,
                                                               target_proc,
                                                               task.regions[idx],
                                                               mem_constraint);
    if (task.regions[idx].privilege == LEGION_REDUCE) {
      // std::cout << "Considering region requirement: " << Utilities::to_string(runtime, ctx, task.regions[idx], idx) << std::endl;
      // auto key = std::make_tuple(task.regions[idx].region, size_t(idx), target_memory);
      // auto key = std::make_tuple(task.index_point[0], size_t(idx), target_memory);
      auto key = std::make_tuple(task_hash, size_t(idx), target_memory);
      // std::cout << "KEY: " << std::get<0>(key) << " " << std::get<1>(key) << " " << std::get<2>(key) << std::endl;
      auto it = this->reductionInstanceCache.find(key);
      if (it != this->reductionInstanceCache.end()) {
        std::vector<PhysicalInstance> instances = it->second;
        // std::cout << "Potential reduction instances: ";
        // for (auto inst : instances) {
        //   std::cout << Utilities::to_string(runtime, ctx, inst) << " ";
        // }
        // std::cout << std::endl;
        if (instances.size() > 0 && this->runtime->acquire_and_filter_instances(ctx, instances)) {
          output.chosen_instances[idx] = instances;
          this->reductionInstanceCache[key] = instances;
          // std::cout << "Reusing instance for regionReq: ";
          // for (auto inst : instances) {
          //   std::cout << Utilities::to_string(runtime, ctx, inst) << " ";
          // }
          // std::cout << std::endl;
          continue;
        } else {
          // std::cout << "Clearing reduction instance cache?" << std::endl;
          this->reductionInstanceCache[key].clear();
        }
      }
    }
    // if (task.regions[idx].privilege == LEGION_REDUCE) {
    //   size_t footprint;
    //   if (!default_create_custom_instances(ctx, target_proc,
    //                                        target_memory, task.regions[idx], idx, missing_fields[idx],
    //                                        layout_constraints, needs_field_constraint_check,
    //                                        output.chosen_instances[idx], &footprint)) {
    //     default_report_failed_instance_creation(task, idx,
    //                                             target_proc, target_memory, footprint);
    //   }
    //   continue;
    // }
    // Did the application request a virtual mapping for this requirement?
    if ((task.regions[idx].tag & DefaultMapper::VIRTUAL_MAP) != 0 && !(task.regions[idx].privilege == LEGION_REDUCE)) {
      PhysicalInstance virt_inst = PhysicalInstance::get_virtual_instance();
      output.chosen_instances[idx].push_back(virt_inst);
      continue;
    }
    // Check to see if any of the valid instances satisfy this requirement
    {
      std::vector<PhysicalInstance> valid_instances;

      for (std::vector<PhysicalInstance>::const_iterator
               it = input.valid_instances[idx].begin(),
               ie = input.valid_instances[idx].end(); it != ie; ++it) {
        if (it->get_location() == target_memory)
          valid_instances.push_back(*it);
      }
      std::set<FieldID> valid_missing_fields;
      runtime->filter_instances(ctx, task, idx, output.chosen_variant,
                                valid_instances, valid_missing_fields);

#ifndef NDEBUG
      bool check =
#endif
          runtime->acquire_and_filter_instances(ctx, valid_instances);
      assert(check);

      output.chosen_instances[idx] = valid_instances;
      missing_fields[idx] = valid_missing_fields;

      if (missing_fields[idx].empty()) {
        continue;
      }
    }
    // Otherwise make normal instances for the given region
    size_t footprint;
    if (!default_create_custom_instances(ctx, target_proc,
                                         target_memory, task.regions[idx], idx, missing_fields[idx],
                                         layout_constraints, needs_field_constraint_check,
                                         output.chosen_instances[idx], &footprint)) {
      default_report_failed_instance_creation(task, idx,
                                              target_proc, target_memory, footprint);
    }
    if (task.regions[idx].privilege == LEGION_REDUCE) {
      // auto key = std::make_tuple(task.regions[idx].region, size_t(idx), target_memory);
      // auto key = std::make_tuple(task.index_point[0], size_t(idx), target_memory);
      auto key = std::make_tuple(task_hash, size_t(idx), target_memory);
      // auto key = std::make_tuple(task_hash, size_t(idx), target_memory);
      this->reductionInstanceCache[key] = output.chosen_instances[idx];
    }
  }

  // Finally we set a target memory for output instances
  Memory target_memory =
      default_policy_select_output_target(ctx, task.target_proc);
  for (unsigned i = 0; i < task.output_regions.size(); ++i) {
    output.output_targets[i] = target_memory;
    default_policy_select_output_constraints(
        task, output.output_constraints[i], task.output_regions[i]);
  }

  if (cache_policy == DEFAULT_CACHE_POLICY_ENABLE && false) {
    // Now that we are done, let's cache the result so we can use it later
    std::list<CachedTaskMapping> &map_list = cached_task_mappings[cache_key];
    map_list.push_back(CachedTaskMapping());
    CachedTaskMapping &cached_result = map_list.back();
    cached_result.task_hash = task_hash;
    cached_result.variant = output.chosen_variant;
    cached_result.mapping = output.chosen_instances;
    cached_result.output_targets = output.output_targets;
    cached_result.output_constraints = output.output_constraints;
  }
}

void TACOMapper::map_task(const Legion::Mapping::MapperContext ctx,
                          const Legion::Task &task,
                          const MapTaskInput &input,
                          MapTaskOutput &output) {
  this->map_task_impl(ctx, task, input, output);
  // If the tag is marked for untracked valid regions, then mark all of its
  // read only regions as up for collection.
  if ((task.tag & UNTRACK_VALID_REGIONS) != 0 && this->untrackValidRegions) {
    for (size_t i = 0; i < task.regions.size(); i++) {
      auto &rg = task.regions[i];
      if (rg.privilege == READ_ONLY && i == 4) {
        output.untracked_valid_regions.insert(i);
      }
    }
  }
  // Mark that we want profiling from this task if we're supposed to backpressure it.
  if ((task.tag & BACKPRESSURE_TASK) != 0 && this->enableBackpressure) {
    output.task_prof_requests.add_measurement<ProfilingMeasurements::OperationStatus>();
  }
}

// default_policy_select_instance_region selects the region to actually allocate
// when given a request to map an instance. The DefaultMapper's policy here is
// generally good, except that it sometimes attempts to allocate instances of the
// root region, which in many cases for DISTAL is of size 2^63. This method is a
// slightly modified version of the DefaultMapper's implementation that makes sure
// to not perform such an allocation.
Legion::LogicalRegion TACOMapper::default_policy_select_instance_region(Legion::Mapping::MapperContext ctx,
                                                                        Legion::Memory target_memory,
                                                                        const Legion::RegionRequirement &req,
                                                                        const Legion::LayoutConstraintSet &constraints,
                                                                        bool force_new_instances,
                                                                        bool meets_constraints) {
  // If it is not something we are making a big region for just
  // return the region that is actually needed.
  LogicalRegion result = req.region;
  if (!meets_constraints || (req.privilege == LEGION_REDUCE))
    return result;

  // If the application requested that we use the exact region requested,
  // then honor that.
  if (exact_region || constraints.specialized_constraint.is_exact() ||
      (req.tag & DefaultMapper::EXACT_REGION) != 0)
    return result;

  // Heuristically use the exact region if the target memory is either a GPU
  // framebuffer or a zero copy memory. Or, if we are putting data in a numa
  // local memory when there are multiple numa memories available, the data
  // is likely partitioned for each numa memory.
  if (target_memory.kind() == Memory::GPU_FB_MEM ||
      target_memory.kind() == Memory::Z_COPY_MEM ||
      (target_memory.kind() == Memory::SOCKET_MEM && this->multipleNumaDomainsPresent))
    return result;

  // Simple heuristic here, if we are on a single node, we go all the
  // way to the root since the first-level partition is likely just
  // across processors in the node, however, if we are on multiple nodes
  // we try to find the first level that effectively partitions the root
  // into one subregion per node.
  if (total_nodes == 1)
  {
    while (runtime->has_parent_logical_partition(ctx, result))
    {
      LogicalPartition parent =
          runtime->get_parent_logical_partition(ctx, result);
      auto parentRegion = runtime->get_parent_logical_region(ctx, parent);
      auto parentRegionBounds = runtime->get_index_space_domain(ctx, parentRegion.get_index_space());
      bool isInfty = false;
      for (int i = 0; i < parentRegionBounds.dim; i++) {
        if (parentRegionBounds.hi() == LEGION_TENSOR_INFTY) {
          isInfty = true;
          break;
        }
      }
      // If the parent region is infinitely sized, then we don't want to
      // go up the region tree anymore.
      if (isInfty) return result;
      result = parentRegion;
    }
    return result;
  }
  else
  {
    // Fall through if the application actually asked for the root.
    if (!runtime->has_parent_logical_partition(ctx, result))
      return result;

    std::vector<LogicalRegion> path;
    std::vector<size_t> volumes;

    path.push_back(result);
    volumes.push_back(runtime->get_index_space_domain(ctx,
                                                      result.get_index_space()).get_volume());

    // Collect the size of subregion at each level.
    LogicalRegion next = result;
    while (runtime->has_parent_logical_partition(ctx, next))
    {
      LogicalPartition parent =
          runtime->get_parent_logical_partition(ctx, next);
      next = runtime->get_parent_logical_region(ctx, parent);
      path.push_back(next);
      volumes.push_back(
          runtime->get_index_space_domain(ctx, next.get_index_space()).get_volume());
    }

    // Accumulate the "effective" fan-out at each level and
    // stop the search once we have one subregion per node.
    double effective_fanout = 1.0;
    for (off_t idx = (off_t)path.size() - 2; idx >= 0; --idx)
    {
      effective_fanout *= (double)volumes[idx + 1] / volumes[idx];
      if ((unsigned)effective_fanout >= total_nodes)
        return path[idx];
    }

    // If we reached this point, the partitions were not meant to assign
    // one subregion per node. So, stop pretending to be smart and
    // just return the exact target.
    return result;
  }
}

// map_replicate_task is overridden for situations where we are running
// multiple target processors per node and need to arrange them into
// the multi-dimensional grids of our choice. In these cases, we want to
// use multiple shards per node (i.e. 1 shard per target processor).
void TACOMapper::map_replicate_task(const Legion::Mapping::MapperContext ctx, const Legion::Task &task,
                                    const MapTaskInput &input, const MapTaskOutput &default_output,
                                    MapReplicateTaskOutput &output) {
  // If we aren't expected to run multiple shards per node, then just
  // fall back to the default mapper.
  if (!this->multipleShardsPerNode) {
    DefaultMapper::map_replicate_task(ctx, task, input, default_output, output);
    return;
  }

  // We should only be mapping the top level task.
  taco_iassert((task.get_depth() == 0) && (task.regions.size() == 0));
  taco_iassert(task.target_proc.kind() == Processor::LOC_PROC);
  auto targetKind = Processor::LOC_PROC;
  const auto chosen = default_find_preferred_variant(task, ctx, true /* needs tight bound */, true /* cache */, targetKind);
  taco_iassert(chosen.is_replicable);
  // Collect all LOC_PROC's to put shards on.
  Legion::Machine::ProcessorQuery cpuQuery(this->machine);
  cpuQuery.only_kind(targetKind);
  auto allCPUs = std::vector<Processor>(cpuQuery.begin(), cpuQuery.end());

  // Create a shard for each CPU processor.
  output.task_mappings.resize(allCPUs.size());
  output.control_replication_map.resize(allCPUs.size());
  for (size_t i = 0; i < allCPUs.size(); i++) {
    output.task_mappings[i].target_procs.push_back(allCPUs[i]);
    output.task_mappings[i].chosen_variant = chosen.variant;
    output.control_replication_map[i] = allCPUs[i];
  }
}

LayoutConstraintID TACOMapper::default_policy_select_layout_constraints(
    MapperContext ctx, Memory target_memory,
    const RegionRequirement &req,
    MappingKind mapping_kind,
    bool needs_field_constraint_check,
    bool &force_new_instances) {
  // Do something special for reductions and
  // it is not an explicit region-to-region copy
  if ((req.privilege == LEGION_REDUCE) && (mapping_kind != COPY_MAPPING)) {
    // Always make new reduction instances
    force_new_instances = true;
    std::tuple<Memory::Kind, IndexSpace, FieldSpace, ReductionOpID> constraint_key(target_memory.kind(),
                                                                                   req.region.get_index_space(),
                                                                                   req.region.get_field_space(),
                                                                                   req.redop);
    auto finder = this->distalReductionConstraintCache.find(constraint_key);
    // No need to worry about field constraint checks here
    // since we don't actually have any field constraints
    if (finder != this->distalReductionConstraintCache.end())
      return finder->second;
    LayoutConstraintSet constraints;
    default_policy_select_constraints(ctx, constraints, target_memory, req);
    LayoutConstraintID result =
        runtime->register_layout(ctx, constraints);
    // Save the result
    this->distalReductionConstraintCache[constraint_key] = result;
    return result;
  }
  // We always set force_new_instances to false since we are
  // deciding to optimize for minimizing memory usage instead
  // of avoiding Write-After-Read (WAR) dependences
  force_new_instances = false;
  // See if we've already made a constraint set for this layout
  std::tuple<Memory::Kind, IndexSpace, FieldSpace> constraint_key(target_memory.kind(), req.region.get_index_space(),
                                                                  req.region.get_field_space());
  auto finder = this->distalLayoutConstraintCache.find(constraint_key);
  if (finder != this->distalLayoutConstraintCache.end()) {
    // If we don't need a constraint check we are already good
    if (!needs_field_constraint_check)
      return finder->second;
    // Check that the fields still are the same, if not, fall through
    // so that we make a new set of constraints
    const LayoutConstraintSet &old_constraints =
        runtime->find_layout_constraints(ctx, finder->second);
    // Should be only one unless things have changed
    const std::vector<FieldID> &old_set =
        old_constraints.field_constraint.get_field_set();
    // Check to make sure the field sets are still the same
    std::vector<FieldID> new_fields;
    runtime->get_field_space_fields(ctx, std::get<2>(constraint_key), new_fields);
    if (new_fields.size() == old_set.size()) {
      std::set<FieldID> old_fields(old_set.begin(), old_set.end());
      bool still_equal = true;
      for (unsigned idx = 0; idx < new_fields.size(); idx++) {
        if (old_fields.find(new_fields[idx]) == old_fields.end()) {
          still_equal = false;
          break;
        }
      }
      if (still_equal)
        return finder->second;
    }
    // Otherwise we fall through and make a new constraint which
    // will also update the cache
  }
  // Fill in the constraints
  LayoutConstraintSet constraints;
  default_policy_select_constraints(ctx, constraints, target_memory, req);
  // Do the registration
  LayoutConstraintID result =
      runtime->register_layout(ctx, constraints);
  // Record our results, there is a benign race here as another mapper
  // call could have registered the exact same registration constraints
  // here if we were preempted during the registration call. The
  // constraint sets are identical though so it's all good.
  this->distalLayoutConstraintCache[constraint_key] = result;
  return result;
}

void TACOMapper::default_policy_select_constraints(Legion::Mapping::MapperContext ctx,
                                                   Legion::LayoutConstraintSet &constraints,
                                                   Legion::Memory target_memory,
                                                   const Legion::RegionRequirement &req) {
  // Ensure that regions are mapped in row-major order.
  Legion::IndexSpace is = req.region.get_index_space();
  Legion::Domain domain = runtime->get_index_space_domain(ctx, is);
  int dim = domain.get_dim();
  std::vector<Legion::DimensionKind> dimension_ordering(dim + 1);
  for (int i = 0; i < dim; ++i) {
    dimension_ordering[dim - i - 1] =
        static_cast<Legion::DimensionKind>(static_cast<int>(LEGION_DIM_X) + i);
  }
  dimension_ordering[dim] = LEGION_DIM_F;
  constraints.add_constraint(Legion::OrderingConstraint(dimension_ordering, false/*contiguous*/));
  // If we were requested to have an alignment, add the constraint.
  if (this->alignTo128Bytes) {
    for (auto it : req.privilege_fields) {
      constraints.add_constraint(Legion::AlignmentConstraint(it, LEGION_EQ_EK, 128));
    }
  }
  // If the instance is supposed to be sparse, tell Legion we want it that way.
  // Unfortunately because we are adjusting the SpecializedConstraint, we have to
  // fully override the default mapper because there appears to be some undefined
  // behavior when two specialized constraints are added.
  if ((req.tag & SPARSE_INSTANCE) != 0) {
    taco_iassert(req.privilege != LEGION_REDUCE);
    constraints.add_constraint(SpecializedConstraint(LEGION_COMPACT_SPECIALIZE));
  } else if (req.privilege == LEGION_REDUCE) {
    // Make reduction fold instances.
    constraints.add_constraint(SpecializedConstraint(
                        LEGION_AFFINE_REDUCTION_SPECIALIZE, req.redop))
      .add_constraint(MemoryConstraint(target_memory.kind()));
  } else {
    // Our base default mapper will try to make instances of containing
    // all fields (in any order) laid out in SOA format to encourage
    // maximum re-use by any tasks which use subsets of the fields
    constraints.add_constraint(SpecializedConstraint())
               .add_constraint(MemoryConstraint(target_memory.kind()));
    if (constraints.field_constraint.field_set.size() == 0)
    {
      // Normal instance creation
      std::vector<FieldID> fields;
      default_policy_select_constraint_fields(ctx, req, fields);
      constraints.add_constraint(FieldConstraint(fields,false/*contiguous*/,false/*inorder*/));
    }
  }
}

void TACOMapper::default_policy_select_target_processors(
    Legion::Mapping::MapperContext ctx,
    const Legion::Task &task,
    std::vector<Legion::Processor> &target_procs) {
  // TODO (rohany): Add a TACO tag to the tasks.
  if (task.is_index_space) {
    // Index launches should be placed directly on the processor
    // they were sliced to.
    target_procs.push_back(task.target_proc);
  } else if (std::string(task.get_task_name()).find("task_") != std::string::npos) {
    // Other point tasks should stay on the originating processor, if they are
    // using a CPU Proc. Otherwise, send the tasks where the default mapper
    // says they should go. I think that the heuristics for OMP_PROC and TOC_PROC
    // are correct for our use case.
    if (task.target_proc.kind() == task.orig_proc.kind()) {
      target_procs.push_back(task.orig_proc);
    } else {
      DefaultMapper::default_policy_select_target_processors(ctx, task, target_procs);
    }
  } else {
    DefaultMapper::default_policy_select_target_processors(ctx, task, target_procs);
  }
}

Memory TACOMapper::default_policy_select_target_memory(Legion::Mapping::MapperContext ctx,
                                                       Legion::Processor target_proc,
                                                       const Legion::RegionRequirement &req,
                                                       Legion::MemoryConstraint mc) {
  // If we are supposed to perform NUMA aware allocations
  if (target_proc.kind() == Processor::OMP_PROC && this->numaAwareAllocs) {
    auto it = this->numaDomains.find(target_proc);
    taco_iassert(it != this->numaDomains.end());
    return it->second;
  } else {
    return DefaultMapper::default_policy_select_target_memory(ctx, target_proc, req, mc);
  }
}

int TACOMapper::default_policy_select_garbage_collection_priority(Legion::Mapping::MapperContext ctx, MappingKind kind,
                                                                  Legion::Memory memory,
                                                                  const Legion::Mapping::PhysicalInstance &instance,
                                                                  bool meets_fill_constraints, bool reduction) {
  // Copy the default mapper's heuristic to eagerly collection reduction instances.
  if (reduction) {
    return LEGION_GC_FIRST_PRIORITY;
  }
  // Deviate from the default mapper to give all instances default GC priority. The
  // default mapper most of the time marks instances as un-collectable from the GC,
  // which leads to problems when using instances in a "temporary buffer" style.
  return LEGION_GC_DEFAULT_PRIORITY;
}

std::vector<Legion::Processor> TACOMapper::select_targets_for_task(const Legion::Mapping::MapperContext ctx,
                                                       const Legion::Task& task) {
  auto kind = this->default_find_preferred_variant(task, ctx, false /* needs tight bounds */).proc_kind;
  // If we're requested to fill/validate on the CPU, then hijack the initial
  // processor selection to do so.
  // TODO (rohany): Do the management for the dummy read task here.
  if ((this->preferCPUFill && task.task_id == TID_TACO_FILL_TASK) ||
      (this->preferCPUValidate && task.task_id == TID_TACO_VALIDATE_TASK)) {
    // See if we have any OMP procs.
    auto targetKind = Legion::Processor::Kind::LOC_PROC;
    Legion::Machine::ProcessorQuery omps(this->machine);
    omps.only_kind(Legion::Processor::OMP_PROC);
    if (omps.count() > 0) {
      targetKind = Legion::Processor::Kind::OMP_PROC;
    }
    kind = targetKind;
  } else if ((task.tag & MAP_TO_OMP_OR_LOC) != 0) {
    // Map our task onto OMP or LOC processors.
    if (!this->local_omps.empty() && this->have_proc_kind_variant(ctx, task.task_id, Processor::OMP_PROC)) {
      kind = Processor::OMP_PROC;
    } else {
      taco_iassert(!this->local_cpus.empty());
      kind = Processor::LOC_PROC;
    }
  }

  // If we're running with multiple shards per node, then we already have a decomposition of tasks
  // onto each shard. So, just return the assigned processor. Note that we only do this for tasks
  // that are being sharded -- i.e. have depth 1 and are index space launches.
  if (this->multipleShardsPerNode && !this->shardCPUGPUMapping.empty() && task.get_depth() == 1 && task.is_index_space) {
    auto targetProc = this->shardCPUGPUMapping[task.orig_proc];
    taco_iassert(kind == targetProc.kind());
    return std::vector<Processor>{targetProc};
  }

  // We always map to the same address space if replication is enabled.
  auto sameAddressSpace = ((task.tag & DefaultMapper::SAME_ADDRESS_SPACE) != 0) || this->replication_enabled;
  if (sameAddressSpace) {
    // If we are meant to stay local, then switch to return the appropriate
    // cached processors.
    switch (kind) {
      case Legion::Processor::OMP_PROC: {
        return this->local_omps;
      }
      case Legion::Processor::TOC_PROC: {
        return this->local_gpus;
      }
      case Legion::Processor::LOC_PROC: {
        return this->local_cpus;
      }
      default: {
        taco_iassert(false);
      }
    }
  } else {
    // If we are meant to distribute over all of the processors, then run a query
    // to find all processors of the desired kind.
    Legion::Machine::ProcessorQuery all_procs(machine);
    all_procs.only_kind(kind);
    return std::vector<Legion::Processor>(all_procs.begin(), all_procs.end());
  }

  // Keep the compiler happy.
  taco_iassert(false);
  return {};
}

void TACOMapper::slice_task(const Legion::Mapping::MapperContext ctx,
                            const Legion::Task &task,
                            const SliceTaskInput &input,
                            SliceTaskOutput &output) {
  if (task.tag & PLACEMENT) {
    // Placement tasks will put the dimensions of the placement grid at the beginning
    // of the task arguments. Here, we extract the packed placement grid dimensions.
    int dim = input.domain.get_dim();
    int *args = (int *) (task.args);
    std::vector<int> gridDims(dim);
    for (int i = 0; i < dim; i++) {
      gridDims[i] = args[i];
    }
    auto targets = this->select_targets_for_task(ctx, task);
    switch (dim) {
#define BLOCK(DIM) \
        case DIM:  \
          {        \
            Legion::DomainT<DIM, Legion::coord_t> pointSpace = input.domain; \
            this->decompose_points(pointSpace, gridDims, targets, output.slices);        \
            break;   \
          }
      LEGION_FOREACH_N(BLOCK)
#undef BLOCK
      default:
        taco_iassert(false);
    }
  } else {
    // Otherwise, we have our own implementation of slice task. The reason for this is
    // because the default mapper gets confused and hits a cache of domain slices. This
    // messes up the placement that we are going for with the index launches. This
    // implementation mirrors the standard slicing strategy of the default mapper.
    auto targets = this->select_targets_for_task(ctx, task);
    switch (input.domain.get_dim()) {
#define BLOCK(DIM) \
        case DIM:  \
          {        \
            Legion::DomainT<DIM,Legion::coord_t> point_space = input.domain; \
            Legion::Point<DIM,Legion::coord_t> num_blocks = \
              default_select_num_blocks<DIM>(targets.size(), point_space.bounds); \
            this->default_decompose_points<DIM>(point_space, targets, \
                  num_blocks, false/*recurse*/, \
                  stealing_enabled, output.slices); \
            break;   \
          }
      LEGION_FOREACH_N(BLOCK)
#undef BLOCK
      default:
        taco_iassert(false);
    }
  }
}

void TACOMapper::report_profiling(const MapperContext ctx,
                                  const Task& task,
                                  const TaskProfilingInfo& input) {
  // We should only get profiling responses if we've enabled backpressuring.
  taco_iassert(this->enableBackpressure);
  // We should only get profiling responses for tasks that are supposed to be backpressured.
  taco_iassert((task.tag & BACKPRESSURE_TASK) != 0);
  auto prof = input.profiling_responses.get_measurement<ProfilingMeasurements::OperationStatus>();
  // All our tasks should complete successfully.
  taco_iassert(prof->result == Realm::ProfilingMeasurements::OperationStatus::COMPLETED_SUCCESSFULLY);
  // Clean up after ourselves.
  delete prof;
  // Backpressured tasks are launched in a loop, and are kept on the originating processor.
  // So, we'll use orig_proc to index into the queue.
  auto& inflight = this->backPressureQueue[task.orig_proc];
  MapperEvent event;
  // Find this task in the queue.
  for (auto it = inflight.begin(); it != inflight.end(); it++) {
    if (it->id == task.get_unique_id()) {
      event = it->event;
      inflight.erase(it);
      break;
    }
  }
  // Assert that we found a valid event.
  taco_iassert(event.exists());
  // Finally, trigger the event for anyone waiting on it.
  this->runtime->trigger_mapper_event(ctx, event);
}

// In select_tasks_to_map, we attempt to perform backpressuring on tasks that
// need to be backpressured.
void TACOMapper::select_tasks_to_map(const MapperContext ctx,
                                     const SelectMappingInput& input,
                                           SelectMappingOutput& output) {
  if (!this->enableBackpressure) {
    DefaultMapper::select_tasks_to_map(ctx, input, output);
  } else {
    // Mark when we are potentially scheduling tasks.
    auto schedTime = std::chrono::high_resolution_clock::now();
    // Create an event that we will return in case we schedule nothing.
    MapperEvent returnEvent;
    // Also maintain a time point of the best return event. We want this function
    // to get invoked as soon as any backpressure task finishes, so we'll use the
    // completion event for the earliest one.
    auto returnTime = std::chrono::high_resolution_clock::time_point::max();

    // Find the depth of the deepest task.
    int max_depth = 0;
    for (std::list<const Task*>::const_iterator it =
        input.ready_tasks.begin(); it != input.ready_tasks.end(); it++)
    {
      int depth = (*it)->get_depth();
      if (depth > max_depth)
        max_depth = depth;
    }
    unsigned count = 0;
    // Only schedule tasks from the max depth in any pass.
    for (std::list<const Task*>::const_iterator it =
        input.ready_tasks.begin(); (count < max_schedule_count) &&
                                   (it != input.ready_tasks.end()); it++)
    {
      auto task = *it;
      bool schedule = true;
      if ((task->tag & BACKPRESSURE_TASK) != 0) {
        // See how many tasks we have in flight. Again, we use the orig_proc here
        // rather than target_proc to match with our heuristics for where serial task
        // launch loops go.
        auto inflight = this->backPressureQueue[task->orig_proc];
        if (inflight.size() == this->maxInFlightTasks) {
          // We've hit the cap, so we can't schedule any more tasks.
          schedule = false;
          // As a heuristic, we'll wait on the first mapper event to
          // finish, as it's likely that one will finish first. We'll also
          // try to get a task that will complete before the current best.
          auto front = inflight.front();
          if (front.schedTime < returnTime) {
            returnEvent = front.event;
            returnTime = front.schedTime;
          }
        } else {
          // Otherwise, we can schedule the task. Create a new event
          // and queue it up on the processor.
          this->backPressureQueue[task->orig_proc].push_back({
            .id = task->get_unique_id(),
            .event = this->runtime->create_mapper_event(ctx),
            .schedTime = schedTime,
          });
        }
      }
      // Schedule tasks that are valid and have the target depth.
      if (schedule && (*it)->get_depth() == max_depth)
      {
        output.map_tasks.insert(*it);
        count++;
      }
    }
    // If we didn't schedule any tasks, tell the runtime to ask us again when
    // our return event triggers.
    if (output.map_tasks.empty()) {
      assert(returnEvent.exists());
      output.deferral_event = returnEvent;
    }
  }
}

Mapper::MapperSyncModel TACOMapper::get_mapper_sync_model() const {
  // If we're going to attempt to backpressure tasks, then we need to use
  // a sync model with high gaurantees.
  if (this->enableBackpressure) {
    return SERIALIZED_NON_REENTRANT_MAPPER_MODEL;
  }
  // Otherwise, we can do whatever the default mapper is doing.
  return DefaultMapper::get_mapper_sync_model();
}


void TACOMapper::map_partition(const MapperContext ctx,
                               const Partition &partition,
                               const MapPartitionInput &input,
                               MapPartitionOutput &output) {
  // No constraints on mapping partitions.
  // Copy over all the valid instances, then try to do an acquire on them
  // and see which instances are no longer valid.
  output.chosen_instances = input.valid_instances;
  if (!output.chosen_instances.empty())
    runtime->acquire_and_filter_instances(ctx, output.chosen_instances);
  // Now see if we have any fields which we still make space for.
  std::vector<unsigned> to_erase;
  std::set<FieldID> missing_fields =
      partition.requirement.privilege_fields;
  for (auto it = output.chosen_instances.begin(); it != output.chosen_instances.end(); it++) {
    if (it->get_location().kind() == Memory::GPU_FB_MEM || it->get_location().kind() == Memory::HDF_MEM) {
      // These instances are not supported yet (see Legion issue #516).
      to_erase.push_back(it - output.chosen_instances.begin());
    } else {
      it->remove_space_fields(missing_fields);
      if (missing_fields.empty())
        break;
    }
  }
  // Erase undesired instances.
  for (auto it = to_erase.rbegin(); it != to_erase.rend(); it++)
    output.chosen_instances.erase((*it) + output.chosen_instances.begin());
  // If we've satisfied all our fields, then we are done.
  if (missing_fields.empty())
    return;
  // Otherwise, let's make an instance for our missing fields.
  Memory target_memory = default_policy_select_target_memory(ctx,
                                                             partition.parent_task->current_proc,
                                                             partition.requirement);
  bool force_new_instances = false;
  LayoutConstraintID our_layout_id =
      default_policy_select_layout_constraints(ctx, target_memory,
                                               partition.requirement,
                                               PARTITION_MAPPING,
                                               true/*needs check*/,
                                               force_new_instances);
  LayoutConstraintSet creation_constraints =
      runtime->find_layout_constraints(ctx, our_layout_id);
  creation_constraints.add_constraint(
      FieldConstraint(missing_fields, false/*contig*/, false/*inorder*/));
  output.chosen_instances.resize(output.chosen_instances.size() + 1);
  size_t footprint;
  if (!default_make_instance(ctx, target_memory, creation_constraints,
                             output.chosen_instances.back(), PARTITION_MAPPING,
                             force_new_instances, true/*meets*/,
                             partition.requirement, &footprint)) {
    // If we failed to make it that is bad.
    logTacoMapper.error("Default mapper failed allocation of size %zd bytes "
                        "for region requirement of partition in task %s (UID "
                        "%lld) in memory " IDFMT " (%s) for processor " IDFMT " (%s). "
                        "This means the working set of your application is too"
                        " big for the allotted capacity of the given memory "
                        "under the default mapper's mapping scheme. You have "
                        "three choices: ask Realm to allocate more memory, "
                        "write a custom mapper to better manage working sets, "
                        "or find a bigger machine.", footprint,
                        partition.parent_task->get_task_name(),
                        partition.parent_task->get_unique_id(),
                        target_memory.id,
                        Utilities::to_string(target_memory.kind()),
                        partition.parent_task->current_proc.id,
                        Utilities::to_string(partition.parent_task->current_proc.kind())
    );
    assert(false);
  }
}

void TACOMapper::select_tunable_value(const MapperContext ctx,
                                      const Task& task,
                                      const SelectTunableInput& input,
                                      SelectTunableOutput& output) {
  size_t* result = (size_t*)malloc(sizeof(size_t));
  output.value = result;
  output.size = sizeof(result);
  switch (input.tunable_id) {
    case TUNABLE_LOCAL_CPU_MEMS: {
      *result = this->localCPUMems;
      break;
    }
    default: {
      // In this case, fall back to the default mapper. However, we need to clean
      // up after ourselves before doing so.
      free(result);
      DefaultMapper::select_tunable_value(ctx, task, input, output);
    }
  }
}

