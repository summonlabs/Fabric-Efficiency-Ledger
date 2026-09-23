// Fabric Efficiency Ledger - fabric topology, sources, generations, scopes.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "fel/topology.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "fel/serialize.hpp"

namespace fel {
namespace {

template <class Enum, std::size_t N>
[[nodiscard]] std::optional<Enum> lookup(const char* const (&names)[N],
                                         std::string_view name) noexcept {
  for (std::size_t i = 0; i < N; ++i) {
    if (name == names[i]) {
      return static_cast<Enum>(i + 1);
    }
  }
  return std::nullopt;
}

constexpr const char* kProvenanceNames[] = {"real", "synthetic", "replay", "unsupported"};

constexpr const char* kSourceKindNames[] = {"switch-counter", "port-counter",     "queue-counter",
                                            "flow-sampler",   "reservation-controller",
                                            "path-probe",     "synthetic-generator",
                                            "replay-archive", "manual-entry"};

constexpr const char* kGenerationStateNames[] = {"active", "superseded"};

constexpr const char* kScopeKindNames[] = {"fabric", "pod",      "switch", "port",
                                           "queue",  "flow",     "reservation", "other"};

constexpr const char* kResourceKindNames[] = {"port", "queue", "link", "device", "buffer", "other"};

// Half open interval overlap for optional upper bounds.
[[nodiscard]] bool intervals_overlap(TimePoint a_start, const std::optional<TimePoint>& a_end,
                                     TimePoint b_start, const std::optional<TimePoint>& b_end) {
  if (a_end.has_value() && b_start > *a_end) {
    return false;
  }
  if (b_end.has_value() && a_start > *b_end) {
    return false;
  }
  return true;
}

}  // namespace

std::string_view provenance_class_name(ProvenanceClass value) noexcept {
  const auto index = static_cast<std::size_t>(value);
  if (index == 0 || index > kProvenanceClassCount) {
    return "invalid";
  }
  return kProvenanceNames[index - 1];
}

std::optional<ProvenanceClass> provenance_class_from_name(std::string_view name) noexcept {
  return lookup<ProvenanceClass>(kProvenanceNames, name);
}

bool provenance_is_countable(ProvenanceClass value) noexcept {
  return value == ProvenanceClass::Real || value == ProvenanceClass::Synthetic ||
         value == ProvenanceClass::Replay;
}

std::string_view source_kind_name(SourceKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index == 0 || index > 9) {
    return "invalid";
  }
  return kSourceKindNames[index - 1];
}

std::optional<SourceKind> source_kind_from_name(std::string_view name) noexcept {
  return lookup<SourceKind>(kSourceKindNames, name);
}

std::string_view generation_state_name(GenerationState state) noexcept {
  const auto index = static_cast<std::size_t>(state);
  if (index == 0 || index > 2) {
    return "invalid";
  }
  return kGenerationStateNames[index - 1];
}

std::string_view scope_kind_name(ScopeKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index == 0 || index > kScopeKindCount) {
    return "invalid";
  }
  return kScopeKindNames[index - 1];
}

std::optional<ScopeKind> scope_kind_from_name(std::string_view name) noexcept {
  return lookup<ScopeKind>(kScopeKindNames, name);
}

std::string_view resource_kind_name(ResourceKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index == 0 || index > 6) {
    return "invalid";
  }
  return kResourceKindNames[index - 1];
}

std::optional<ResourceKind> resource_kind_from_name(std::string_view name) noexcept {
  return lookup<ResourceKind>(kResourceKindNames, name);
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------
Result<void> FabricTopology::add_generation(Generation generation) {
  if (generation.id.is_nil()) {
    return make_error(ErrorCode::InvalidArgument, "generation identity must not be nil");
  }
  if (generation.label.size() > limits::kMaxLabelBytes) {
    return make_error(ErrorCode::MetadataTooLarge, "generation label exceeds the label budget");
  }
  if (generations_.size() >= limits::kMaxGenerations && generations_.find(generation.id) == generations_.end()) {
    return make_error(ErrorCode::CapacityExceeded, "generation budget exhausted");
  }
  if (generations_.find(generation.id) != generations_.end()) {
    return make_error(ErrorCode::DuplicateDefinition, "generation already defined",
                      generation.id.to_string());
  }
  generations_.emplace(generation.id, std::move(generation));
  return ok();
}

Result<void> FabricTopology::add_source(Source source) {
  if (source.id.is_nil()) {
    return make_error(ErrorCode::InvalidArgument, "source identity must not be nil");
  }
  if (source.label.size() > limits::kMaxLabelBytes) {
    return make_error(ErrorCode::MetadataTooLarge, "source label exceeds the label budget");
  }
  if (sources_.size() >= limits::kMaxSources && sources_.find(source.id) == sources_.end()) {
    return make_error(ErrorCode::CapacityExceeded, "source budget exhausted");
  }
  if (sources_.find(source.id) != sources_.end()) {
    return make_error(ErrorCode::DuplicateDefinition, "source already defined",
                      source.id.to_string());
  }
  sources_.emplace(source.id, std::move(source));
  return ok();
}

Result<void> FabricTopology::add_incarnation(SourceIncarnation incarnation) {
  if (incarnation.id.is_nil()) {
    return make_error(ErrorCode::InvalidArgument, "incarnation identity must not be nil");
  }
  if (incarnations_.find(incarnation.id) != incarnations_.end()) {
    return make_error(ErrorCode::DuplicateDefinition, "incarnation already defined",
                      incarnation.id.to_string());
  }
  std::size_t count = 0;
  for (const auto& entry : incarnations_) {
    if (entry.second.source == incarnation.source) {
      ++count;
    }
  }
  if (count >= limits::kMaxIncarnationsPerSource) {
    return make_error(ErrorCode::CapacityExceeded, "incarnation budget exhausted for source",
                      incarnation.source.to_string());
  }
  if (incarnation.retired_at.has_value() && *incarnation.retired_at < incarnation.started_at) {
    return make_error(ErrorCode::InvalidArgument,
                      "incarnation retired before it started", incarnation.id.to_string());
  }
  incarnations_.emplace(incarnation.id, std::move(incarnation));
  return ok();
}

Result<void> FabricTopology::add_scope(Scope scope) {
  if (scope.id.is_nil()) {
    return make_error(ErrorCode::InvalidArgument, "scope identity must not be nil");
  }
  if (scope.label.size() > limits::kMaxLabelBytes) {
    return make_error(ErrorCode::MetadataTooLarge, "scope label exceeds the label budget");
  }
  if (scopes_.size() >= limits::kMaxScopes && scopes_.find(scope.id) == scopes_.end()) {
    return make_error(ErrorCode::CapacityExceeded, "scope budget exhausted");
  }
  if (scopes_.find(scope.id) != scopes_.end()) {
    return make_error(ErrorCode::DuplicateDefinition, "scope already defined", scope.id.to_string());
  }
  if (scope.parent.has_value() && *scope.parent == scope.id) {
    return make_error(ErrorCode::ScopeCycleDetected, "scope cannot be its own parent",
                      scope.id.to_string());
  }
  std::sort(scope.members.begin(), scope.members.end());
  const auto duplicate = std::adjacent_find(scope.members.begin(), scope.members.end());
  if (duplicate != scope.members.end()) {
    return make_error(ErrorCode::DuplicateDefinition, "duplicate scope member",
                      duplicate->to_string());
  }
  scopes_.emplace(scope.id, std::move(scope));
  return ok();
}

Result<void> FabricTopology::add_resource(Resource resource) {
  if (resource.id.is_nil()) {
    return make_error(ErrorCode::InvalidArgument, "resource identity must not be nil");
  }
  if (resource.label.size() > limits::kMaxLabelBytes || resource.locality.size() > limits::kMaxLabelBytes) {
    return make_error(ErrorCode::MetadataTooLarge, "resource label exceeds the label budget");
  }
  if (resources_.size() >= limits::kMaxResources && resources_.find(resource.id) == resources_.end()) {
    return make_error(ErrorCode::CapacityExceeded, "resource budget exhausted");
  }
  if (resources_.find(resource.id) != resources_.end()) {
    return make_error(ErrorCode::DuplicateDefinition, "resource already defined",
                      resource.id.to_string());
  }
  resources_.emplace(resource.id, std::move(resource));
  return ok();
}

Result<void> FabricTopology::add_binding(GenerationBinding binding) {
  if (binding.resource.is_nil() || binding.generation.is_nil()) {
    return make_error(ErrorCode::InvalidArgument, "binding must reference a resource and a generation");
  }
  if (bindings_.size() >= limits::kMaxResources * 4ULL) {
    return make_error(ErrorCode::CapacityExceeded, "generation binding budget exhausted");
  }
  if (binding.valid_to.has_value() && *binding.valid_to < binding.valid_from) {
    return make_error(ErrorCode::InvalidArgument, "binding expires before it becomes valid");
  }
  bindings_.push_back(std::move(binding));
  return ok();
}

Result<void> FabricTopology::add_flow(Flow flow) {
  if (flow.id.is_nil()) {
    return make_error(ErrorCode::InvalidArgument, "flow identity must not be nil");
  }
  if (flow.label.size() > limits::kMaxLabelBytes) {
    return make_error(ErrorCode::MetadataTooLarge, "flow label exceeds the label budget");
  }
  if (flow.path_hint.size() > limits::kMaxResources) {
    return make_error(ErrorCode::TooManyItems, "flow path hint exceeds the item budget");
  }
  if (flows_.size() >= limits::kMaxFlows && flows_.find(flow.id) == flows_.end()) {
    return make_error(ErrorCode::CapacityExceeded, "flow budget exhausted");
  }
  if (flows_.find(flow.id) != flows_.end()) {
    return make_error(ErrorCode::DuplicateDefinition, "flow already defined", flow.id.to_string());
  }
  flows_.emplace(flow.id, std::move(flow));
  return ok();
}

Result<void> FabricTopology::add_path(Path path) {
  if (path.id.is_nil()) {
    return make_error(ErrorCode::InvalidArgument, "path identity must not be nil");
  }
  if (path.label.size() > limits::kMaxLabelBytes) {
    return make_error(ErrorCode::MetadataTooLarge, "path label exceeds the label budget");
  }
  if (path.hops.size() > limits::kMaxResources) {
    return make_error(ErrorCode::TooManyItems, "path hop list exceeds the item budget");
  }
  if (paths_.size() >= limits::kMaxPaths && paths_.find(path.id) == paths_.end()) {
    return make_error(ErrorCode::CapacityExceeded, "path budget exhausted");
  }
  if (paths_.find(path.id) != paths_.end()) {
    return make_error(ErrorCode::DuplicateDefinition, "path already defined", path.id.to_string());
  }
  paths_.emplace(path.id, std::move(path));
  return ok();
}

Result<void> FabricTopology::add_reservation(Reservation reservation) {
  if (reservation.id.is_nil()) {
    return make_error(ErrorCode::InvalidArgument, "reservation identity must not be nil");
  }
  if (reservation.label.size() > limits::kMaxLabelBytes) {
    return make_error(ErrorCode::MetadataTooLarge, "reservation label exceeds the label budget");
  }
  if (reservations_.size() >= limits::kMaxReservations &&
      reservations_.find(reservation.id) == reservations_.end()) {
    return make_error(ErrorCode::CapacityExceeded, "reservation budget exhausted");
  }
  if (reservations_.find(reservation.id) != reservations_.end()) {
    return make_error(ErrorCode::DuplicateDefinition, "reservation already defined",
                      reservation.id.to_string());
  }
  if (reservation.valid_to.has_value() && *reservation.valid_to < reservation.valid_from) {
    return make_error(ErrorCode::InvalidArgument, "reservation expires before it becomes valid");
  }
  reservations_.emplace(reservation.id, std::move(reservation));
  return ok();
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
Result<void> FabricTopology::validate() const {
  if (scopes_.size() > limits::kMaxScopes || resources_.size() > limits::kMaxResources ||
      flows_.size() > limits::kMaxFlows || paths_.size() > limits::kMaxPaths ||
      reservations_.size() > limits::kMaxReservations || sources_.size() > limits::kMaxSources ||
      generations_.size() > limits::kMaxGenerations) {
    return make_error(ErrorCode::CapacityExceeded, "topology exceeds a configured budget");
  }

  for (const auto& entry : scopes_) {
    const Scope& scope = entry.second;
    if (scope.parent.has_value() && scopes_.find(*scope.parent) == scopes_.end()) {
      return make_error(ErrorCode::UnknownScope, "scope references an undefined parent",
                        scope.parent->to_string());
    }
  }

  // Hierarchy must be a forest: walking up from any scope must terminate and
  // must never revisit a scope.
  for (const auto& entry : scopes_) {
    std::vector<ScopeId> seen;
    const Scope* cursor = &entry.second;
    while (cursor->parent.has_value()) {
      if (std::find(seen.begin(), seen.end(), cursor->id) != seen.end()) {
        return make_error(ErrorCode::ScopeCycleDetected, "scope hierarchy contains a cycle",
                          entry.first.to_string());
      }
      seen.push_back(cursor->id);
      const auto parent = scopes_.find(*cursor->parent);
      if (parent == scopes_.end()) {
        return make_error(ErrorCode::UnknownScope, "scope references an undefined parent",
                          cursor->parent->to_string());
      }
      cursor = &parent->second;
    }
  }

  // Every resource lives in exactly one scope, and every member must exist.
  {
    std::map<ResourceId, ScopeId> owners;
    for (const auto& entry : scopes_) {
      for (const auto& member : entry.second.members) {
        if (resources_.find(member) == resources_.end()) {
          return make_error(ErrorCode::UnknownResource, "scope lists an undefined resource",
                            member.to_string());
        }
        const auto inserted = owners.emplace(member, entry.first);
        if (!inserted.second) {
          return make_error(ErrorCode::ResourceInMultipleScopes,
                            "resource is a member of more than one scope", member.to_string());
        }
      }
    }
    for (const auto& entry : resources_) {
      const auto owner = owners.find(entry.first);
      if (owner == owners.end()) {
        return make_error(ErrorCode::UnknownScope, "resource is not a member of any scope",
                          entry.first.to_string());
      }
      if (owner->second != entry.second.scope) {
        return make_error(ErrorCode::ResourceInMultipleScopes,
                          "resource scope disagrees with its declaring scope",
                          entry.first.to_string());
      }
    }
  }

  for (const auto& entry : incarnations_) {
    if (sources_.find(entry.second.source) == sources_.end()) {
      return make_error(ErrorCode::UnknownSource, "incarnation references an undefined source",
                        entry.second.source.to_string());
    }
  }
  // Incarnation intervals must not overlap within a source: two live
  // incarnations of the same reporting agent make sequence fencing unsound.
  {
    std::map<SourceId, std::vector<const SourceIncarnation*>> by_source;
    for (const auto& entry : incarnations_) {
      by_source[entry.second.source].push_back(&entry.second);
    }
    for (auto& pair : by_source) {
      auto& list = pair.second;
      std::sort(list.begin(), list.end(), [](const SourceIncarnation* a, const SourceIncarnation* b) {
        if (a->started_at != b->started_at) return a->started_at < b->started_at;
        return a->id < b->id;
      });
      for (std::size_t i = 1; i < list.size(); ++i) {
        if (intervals_overlap(list[i - 1]->started_at, list[i - 1]->retired_at, list[i]->started_at,
                              list[i]->retired_at)) {
          return make_error(ErrorCode::OverlappingIncarnations,
                            "two incarnations of the same source are live at the same time",
                            list[i]->source.to_string());
        }
      }
    }
  }

  for (const auto& binding : bindings_) {
    if (resources_.find(binding.resource) == resources_.end()) {
      return make_error(ErrorCode::UnknownResource, "binding references an undefined resource",
                        binding.resource.to_string());
    }
    if (generations_.find(binding.generation) == generations_.end()) {
      return make_error(ErrorCode::UnknownGeneration, "binding references an undefined generation",
                        binding.generation.to_string());
    }
  }
  {
    std::vector<const GenerationBinding*> sorted;
    sorted.reserve(bindings_.size());
    for (const auto& binding : bindings_) {
      sorted.push_back(&binding);
    }
    std::sort(sorted.begin(), sorted.end(), [](const GenerationBinding* a, const GenerationBinding* b) {
      if (a->resource != b->resource) return a->resource < b->resource;
      if (a->generation != b->generation) return a->generation < b->generation;
      if (a->valid_from != b->valid_from) return a->valid_from < b->valid_from;
      return a->id < b->id;
    });
    for (std::size_t i = 1; i < sorted.size(); ++i) {
      if (sorted[i - 1]->resource == sorted[i]->resource &&
          sorted[i - 1]->generation == sorted[i]->generation &&
          intervals_overlap(sorted[i - 1]->valid_from, sorted[i - 1]->valid_to, sorted[i]->valid_from,
                            sorted[i]->valid_to)) {
        return make_error(ErrorCode::DuplicateDefinition,
                          "resource has overlapping generation bindings for one generation",
                          sorted[i]->resource.to_string());
      }
    }
  }

  for (const auto& entry : flows_) {
    const Flow& flow = entry.second;
    if (scopes_.find(flow.scope) == scopes_.end()) {
      return make_error(ErrorCode::UnknownScope, "flow references an undefined scope",
                        flow.scope.to_string());
    }
    if (generations_.find(flow.generation) == generations_.end()) {
      return make_error(ErrorCode::UnknownGeneration, "flow references an undefined generation",
                        flow.generation.to_string());
    }
    for (const auto& resource : flow.path_hint) {
      if (resources_.find(resource) == resources_.end()) {
        return make_error(ErrorCode::UnknownResource, "flow references an undefined resource",
                          resource.to_string());
      }
    }
  }

  for (const auto& entry : paths_) {
    const Path& path = entry.second;
    if (scopes_.find(path.scope) == scopes_.end()) {
      return make_error(ErrorCode::UnknownScope, "path references an undefined scope",
                        path.scope.to_string());
    }
    if (generations_.find(path.generation) == generations_.end()) {
      return make_error(ErrorCode::UnknownGeneration, "path references an undefined generation",
                        path.generation.to_string());
    }
    for (const auto& resource : path.hops) {
      if (resources_.find(resource) == resources_.end()) {
        return make_error(ErrorCode::UnknownResource, "path references an undefined resource",
                          resource.to_string());
      }
    }
  }

  for (const auto& entry : reservations_) {
    const Reservation& reservation = entry.second;
    if (scopes_.find(reservation.scope) == scopes_.end()) {
      return make_error(ErrorCode::UnknownScope, "reservation references an undefined scope",
                        reservation.scope.to_string());
    }
    if (generations_.find(reservation.generation) == generations_.end()) {
      return make_error(ErrorCode::UnknownGeneration,
                        "reservation references an undefined generation",
                        reservation.generation.to_string());
    }
    if (reservation.resource.has_value() &&
        resources_.find(*reservation.resource) == resources_.end()) {
      return make_error(ErrorCode::UnknownResource, "reservation references an undefined resource",
                        reservation.resource->to_string());
    }
  }

  return ok();
}

Result<void> FabricTopology::check_resource_membership(const ResourceId& resource,
                                                       const ScopeId& scope,
                                                       const ScopeId* ignore_scope) const {
  for (const auto& entry : scopes_) {
    if (ignore_scope != nullptr && entry.first == *ignore_scope) {
      continue;
    }
    if (std::find(entry.second.members.begin(), entry.second.members.end(), resource) !=
        entry.second.members.end()) {
      return make_error(ErrorCode::ResourceInMultipleScopes,
                        "resource already belongs to scope " + entry.first.to_string(),
                        resource.to_string() + " -> " + scope.to_string());
    }
  }
  return ok();
}

// ---------------------------------------------------------------------------
// Lookup
// ---------------------------------------------------------------------------
const Source* FabricTopology::find_source(const SourceId& id) const noexcept {
  const auto it = sources_.find(id);
  return it == sources_.end() ? nullptr : &it->second;
}

const SourceIncarnation* FabricTopology::find_incarnation(const IncarnationId& id) const noexcept {
  const auto it = incarnations_.find(id);
  return it == incarnations_.end() ? nullptr : &it->second;
}

const Generation* FabricTopology::find_generation(const GenerationId& id) const noexcept {
  const auto it = generations_.find(id);
  return it == generations_.end() ? nullptr : &it->second;
}

const Scope* FabricTopology::find_scope(const ScopeId& id) const noexcept {
  const auto it = scopes_.find(id);
  return it == scopes_.end() ? nullptr : &it->second;
}

const Resource* FabricTopology::find_resource(const ResourceId& id) const noexcept {
  const auto it = resources_.find(id);
  return it == resources_.end() ? nullptr : &it->second;
}

const Flow* FabricTopology::find_flow(const FlowId& id) const noexcept {
  const auto it = flows_.find(id);
  return it == flows_.end() ? nullptr : &it->second;
}

const Path* FabricTopology::find_path(const PathId& id) const noexcept {
  const auto it = paths_.find(id);
  return it == paths_.end() ? nullptr : &it->second;
}

const Reservation* FabricTopology::find_reservation(const ReservationId& id) const noexcept {
  const auto it = reservations_.find(id);
  return it == reservations_.end() ? nullptr : &it->second;
}

bool FabricTopology::generation_binds(const ResourceId& resource, const GenerationId& generation,
                                      TimePoint when) const noexcept {
  for (const auto& binding : bindings_) {
    if (binding.resource != resource || binding.generation != generation) {
      continue;
    }
    if (when < binding.valid_from) {
      continue;
    }
    if (binding.valid_to.has_value() && when > *binding.valid_to) {
      continue;
    }
    return true;
  }
  return false;
}

std::vector<ScopeId> FabricTopology::children_of(const ScopeId& id) const {
  std::vector<ScopeId> out;
  for (const auto& entry : scopes_) {
    if (entry.second.parent.has_value() && *entry.second.parent == id) {
      out.push_back(entry.first);
    }
  }
  return out;
}

std::vector<ScopeId> FabricTopology::descendants_of(const ScopeId& id) const {
  std::vector<ScopeId> out;
  std::vector<ScopeId> frontier{id};
  std::vector<ScopeId> visited{id};
  while (!frontier.empty()) {
    const ScopeId current = frontier.back();
    frontier.pop_back();
    for (const auto& child : children_of(current)) {
      if (std::find(visited.begin(), visited.end(), child) != visited.end()) {
        continue;
      }
      visited.push_back(child);
      out.push_back(child);
      frontier.push_back(child);
    }
    if (out.size() > limits::kMaxScopes) {
      break;
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<ScopeId> FabricTopology::ancestors_of(const ScopeId& id) const {
  std::vector<ScopeId> out;
  const Scope* cursor = find_scope(id);
  std::size_t guard = 0;
  while (cursor != nullptr && cursor->parent.has_value()) {
    if (++guard > limits::kMaxScopes) {
      break;
    }
    const ScopeId parent = *cursor->parent;
    if (std::find(out.begin(), out.end(), parent) != out.end()) {
      break;
    }
    out.push_back(parent);
    cursor = find_scope(parent);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<ResourceId> FabricTopology::resources_of(const ScopeId& id,
                                                     bool include_descendants) const {
  std::vector<ResourceId> out;
  const Scope* scope = find_scope(id);
  if (scope == nullptr) {
    return out;
  }
  out = scope->members;
  if (include_descendants) {
    for (const auto& child : descendants_of(id)) {
      const Scope* child_scope = find_scope(child);
      if (child_scope == nullptr) {
        continue;
      }
      out.insert(out.end(), child_scope->members.begin(), child_scope->members.end());
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::vector<ScopeId> FabricTopology::all_scope_ids() const {
  std::vector<ScopeId> out;
  out.reserve(scopes_.size());
  for (const auto& entry : scopes_) {
    out.push_back(entry.first);
  }
  return out;
}

std::vector<GenerationId> FabricTopology::all_generation_ids() const {
  std::vector<GenerationId> out;
  out.reserve(generations_.size());
  for (const auto& entry : generations_) {
    out.push_back(entry.first);
  }
  return out;
}

std::size_t FabricTopology::size() const noexcept {
  return generations_.size() + sources_.size() + incarnations_.size() + scopes_.size() +
         resources_.size() + bindings_.size() + flows_.size() + paths_.size() +
         reservations_.size();
}

std::string FabricTopology::canonical_form() const {
  FieldWriter writer;
  writer.field("topology/1");
  writer.field_u64(static_cast<std::uint64_t>(generations_.size()));
  for (const auto& entry : generations_) {
    writer.field(entry.second.id.to_string());
    writer.field_u64(entry.second.ordinal.value());
    writer.field(entry.second.label);
    writer.field_i64(entry.second.effective_from.nanos());
    writer.field(entry.second.effective_to.has_value()
                     ? std::to_string(entry.second.effective_to->nanos())
                     : std::string("-"));
    writer.field(generation_state_name(entry.second.state));
  }
  writer.field_u64(static_cast<std::uint64_t>(sources_.size()));
  for (const auto& entry : sources_) {
    writer.field(entry.second.id.to_string());
    writer.field(entry.second.label);
    writer.field(source_kind_name(entry.second.kind));
    writer.field_u64(entry.second.authority.value());
    writer.field(provenance_class_name(entry.second.provenance));
    writer.field_bool(entry.second.enabled);
  }
  writer.field_u64(static_cast<std::uint64_t>(incarnations_.size()));
  for (const auto& entry : incarnations_) {
    writer.field(entry.second.id.to_string());
    writer.field(entry.second.source.to_string());
    writer.field_u64(entry.second.boot_epoch.value());
    writer.field_i64(entry.second.started_at.nanos());
    writer.field(entry.second.retired_at.has_value()
                     ? std::to_string(entry.second.retired_at->nanos())
                     : std::string("-"));
  }
  writer.field_u64(static_cast<std::uint64_t>(scopes_.size()));
  for (const auto& entry : scopes_) {
    writer.field(entry.second.id.to_string());
    writer.field(entry.second.label);
    writer.field(scope_kind_name(entry.second.kind));
    writer.field(entry.second.parent.has_value() ? entry.second.parent->to_string()
                                                 : std::string("-"));
    for (const auto& member : entry.second.members) {
      writer.field(member.to_string());
    }
  }
  writer.field_u64(static_cast<std::uint64_t>(resources_.size()));
  for (const auto& entry : resources_) {
    writer.field(entry.second.id.to_string());
    writer.field(entry.second.label);
    writer.field(resource_kind_name(entry.second.kind));
    writer.field(entry.second.scope.to_string());
    writer.field(entry.second.locality);
  }
  writer.field_u64(static_cast<std::uint64_t>(bindings_.size()));
  for (const auto& entry : bindings_) {
    writer.field(entry.resource.to_string());
    writer.field(entry.generation.to_string());
    writer.field_i64(entry.valid_from.nanos());
    writer.field(entry.valid_to.has_value() ? std::to_string(entry.valid_to->nanos())
                                            : std::string("-"));
  }
  writer.field_u64(static_cast<std::uint64_t>(flows_.size()));
  for (const auto& entry : flows_) {
    writer.field(entry.second.id.to_string());
    writer.field(entry.second.label);
    writer.field(entry.second.scope.to_string());
    writer.field(entry.second.generation.to_string());
    for (const auto& resource : entry.second.path_hint) {
      writer.field(resource.to_string());
    }
  }
  writer.field_u64(static_cast<std::uint64_t>(paths_.size()));
  for (const auto& entry : paths_) {
    writer.field(entry.second.id.to_string());
    writer.field(entry.second.label);
    writer.field(entry.second.scope.to_string());
    writer.field(entry.second.generation.to_string());
    for (const auto& resource : entry.second.hops) {
      writer.field(resource.to_string());
    }
  }
  writer.field_u64(static_cast<std::uint64_t>(reservations_.size()));
  for (const auto& entry : reservations_) {
    writer.field(entry.second.id.to_string());
    writer.field(entry.second.label);
    writer.field(entry.second.scope.to_string());
    writer.field(entry.second.generation.to_string());
    writer.field(entry.second.resource.has_value() ? entry.second.resource->to_string()
                                                   : std::string("-"));
    writer.field_u64(entry.second.reserved_capacity_nanos);
    writer.field_i64(entry.second.valid_from.nanos());
    writer.field(entry.second.valid_to.has_value() ? std::to_string(entry.second.valid_to->nanos())
                                                   : std::string("-"));
  }
  return writer.text();
}

Digest256 FabricTopology::digest() const { return Sha256::hash(canonical_form()); }

TopologyRevisionId FabricTopology::revision_id() const {
  return TopologyRevisionId::derive({"topology", digest().to_hex()});
}

}  // namespace fel
