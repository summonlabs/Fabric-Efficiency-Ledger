// Fabric Efficiency Ledger - JSON codecs for the topology document.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "topology_json.hpp"

#include <string>

#include "fel/limits.hpp"
#include "fel/version.hpp"

namespace fel {
namespace detail {
namespace {

[[nodiscard]] Json optional_time_json(const std::optional<TimePoint>& value) {
  return Json::string(value.has_value() ? value->to_rfc3339_millis() : std::string("-"));
}

[[nodiscard]] Result<std::optional<TimePoint>> parse_optional_time(const Json& document,
                                                                   const char* key) {
  const Json* value = document.find(key);
  if (value == nullptr || !value->is_string() || value->as_string() == "-") {
    return std::optional<TimePoint>{};
  }
  FEL_TRY_ASSIGN(const TimePoint parsed, parse_rfc3339(value->as_string()));
  return std::optional<TimePoint>{parsed};
}

[[nodiscard]] Result<ResourceId> resource_id(const Json& document, const char* key) {
  FEL_TRY_ASSIGN(const std::string text, document.require_string(key));
  return ResourceId::parse(text);
}

}  // namespace

Json topology_to_json(const FabricTopology& topology) {
  Json root = Json::object();
  root.set("format_version", Json::number(static_cast<std::uint64_t>(kTopologyFormatVersion)));

  Json generations = Json::array();
  for (const auto& entry : topology.generations()) {
    Json item = Json::object();
    item.set("id", Json::string(entry.second.id.to_string()));
    item.set("ordinal", Json::number(entry.second.ordinal.value()));
    item.set("label", Json::string(entry.second.label));
    item.set("effective_from", Json::string(entry.second.effective_from.to_rfc3339_millis()));
    item.set("effective_to", optional_time_json(entry.second.effective_to));
    item.set("state", Json::string(std::string(generation_state_name(entry.second.state))));
    generations.push(std::move(item));
  }
  root.set("generations", std::move(generations));

  Json sources = Json::array();
  for (const auto& entry : topology.sources()) {
    Json item = Json::object();
    item.set("id", Json::string(entry.second.id.to_string()));
    item.set("label", Json::string(entry.second.label));
    item.set("kind", Json::string(std::string(source_kind_name(entry.second.kind))));
    item.set("authority", Json::number(entry.second.authority.value()));
    item.set("provenance", Json::string(std::string(provenance_class_name(entry.second.provenance))));
    item.set("enabled", Json::boolean(entry.second.enabled));
    sources.push(std::move(item));
  }
  root.set("sources", std::move(sources));

  Json incarnations = Json::array();
  for (const auto& entry : topology.incarnations()) {
    Json item = Json::object();
    item.set("id", Json::string(entry.second.id.to_string()));
    item.set("source", Json::string(entry.second.source.to_string()));
    item.set("boot_epoch", Json::number(entry.second.boot_epoch.value()));
    item.set("started_at", Json::string(entry.second.started_at.to_rfc3339_millis()));
    item.set("retired_at", optional_time_json(entry.second.retired_at));
    item.set("note", Json::string(entry.second.note));
    incarnations.push(std::move(item));
  }
  root.set("incarnations", std::move(incarnations));

  Json scopes = Json::array();
  for (const auto& entry : topology.scopes()) {
    Json item = Json::object();
    item.set("id", Json::string(entry.second.id.to_string()));
    item.set("label", Json::string(entry.second.label));
    item.set("kind", Json::string(std::string(scope_kind_name(entry.second.kind))));
    item.set("parent", Json::string(entry.second.parent.has_value() ? entry.second.parent->to_string()
                                                                   : std::string("-")));
    Json members = Json::array();
    for (const auto& member : entry.second.members) {
      members.push(Json::string(member.to_string()));
    }
    item.set("members", std::move(members));
    scopes.push(std::move(item));
  }
  root.set("scopes", std::move(scopes));

  Json resources = Json::array();
  for (const auto& entry : topology.resources()) {
    Json item = Json::object();
    item.set("id", Json::string(entry.second.id.to_string()));
    item.set("label", Json::string(entry.second.label));
    item.set("kind", Json::string(std::string(resource_kind_name(entry.second.kind))));
    item.set("scope", Json::string(entry.second.scope.to_string()));
    item.set("locality", Json::string(entry.second.locality));
    resources.push(std::move(item));
  }
  root.set("resources", std::move(resources));

  Json bindings = Json::array();
  for (const auto& entry : topology.bindings()) {
    Json item = Json::object();
    item.set("resource", Json::string(entry.resource.to_string()));
    item.set("generation", Json::string(entry.generation.to_string()));
    item.set("valid_from", Json::string(entry.valid_from.to_rfc3339_millis()));
    item.set("valid_to", optional_time_json(entry.valid_to));
    bindings.push(std::move(item));
  }
  root.set("bindings", std::move(bindings));

  Json flows = Json::array();
  for (const auto& entry : topology.flows()) {
    Json item = Json::object();
    item.set("id", Json::string(entry.second.id.to_string()));
    item.set("label", Json::string(entry.second.label));
    item.set("scope", Json::string(entry.second.scope.to_string()));
    item.set("generation", Json::string(entry.second.generation.to_string()));
    Json hint = Json::array();
    for (const auto& value : entry.second.path_hint) {
      hint.push(Json::string(value.to_string()));
    }
    item.set("path_hint", std::move(hint));
    flows.push(std::move(item));
  }
  root.set("flows", std::move(flows));

  Json paths = Json::array();
  for (const auto& entry : topology.paths()) {
    Json item = Json::object();
    item.set("id", Json::string(entry.second.id.to_string()));
    item.set("label", Json::string(entry.second.label));
    item.set("scope", Json::string(entry.second.scope.to_string()));
    item.set("generation", Json::string(entry.second.generation.to_string()));
    Json hops = Json::array();
    for (const auto& value : entry.second.hops) {
      hops.push(Json::string(value.to_string()));
    }
    item.set("hops", std::move(hops));
    paths.push(std::move(item));
  }
  root.set("paths", std::move(paths));

  Json reservations = Json::array();
  for (const auto& entry : topology.reservations()) {
    Json item = Json::object();
    item.set("id", Json::string(entry.second.id.to_string()));
    item.set("label", Json::string(entry.second.label));
    item.set("scope", Json::string(entry.second.scope.to_string()));
    item.set("generation", Json::string(entry.second.generation.to_string()));
    item.set("resource", Json::string(entry.second.resource.has_value()
                                          ? entry.second.resource->to_string()
                                          : std::string("-")));
    item.set("reserved_capacity_nanos", Json::number(entry.second.reserved_capacity_nanos));
    item.set("valid_from", Json::string(entry.second.valid_from.to_rfc3339_millis()));
    item.set("valid_to", optional_time_json(entry.second.valid_to));
    reservations.push(std::move(item));
  }
  root.set("reservations", std::move(reservations));
  return root;
}

Result<FabricTopology> topology_from_json(const Json& document) {
  if (!document.is_object()) {
    return make_error(ErrorCode::SchemaViolation, "topology document must be a JSON object");
  }
  FEL_TRY(document.reject_unknown_keys(
      {"format_version", "generations", "sources", "incarnations", "scopes", "resources",
       "bindings", "flows", "paths", "reservations"}));
  FabricTopology topology;

  const Json* generations = document.find("generations");
  if (generations != nullptr && generations->is_array()) {
    if (generations->as_array().size() > limits::kMaxGenerations) {
      return make_error(ErrorCode::TooManyItems, "generation list exceeds the item budget");
    }
    for (const auto& item : generations->as_array()) {
      Generation generation;
      FEL_TRY_ASSIGN(const std::string id_text, item.require_string("id"));
      FEL_TRY_ASSIGN(generation.id, GenerationId::parse(id_text));
      std::uint64_t ordinal = 0;
      FEL_TRY_ASSIGN(ordinal, item.require_u64("ordinal"));
      generation.ordinal = Ordinal{ordinal};
      FEL_TRY_ASSIGN(generation.label, item.require_string("label"));
      FEL_TRY_ASSIGN(const std::string from_text, item.require_string("effective_from"));
      FEL_TRY_ASSIGN(generation.effective_from, parse_rfc3339(from_text));
      FEL_TRY_ASSIGN(generation.effective_to, parse_optional_time(item, "effective_to"));
      FEL_TRY_ASSIGN(const std::string state_text, item.require_string("state"));
      if (state_text == "active") {
        generation.state = GenerationState::Active;
      } else if (state_text == "superseded") {
        generation.state = GenerationState::Superseded;
      } else {
        return make_error(ErrorCode::InvalidArgument, "unknown generation state", state_text);
      }
      FEL_TRY(topology.add_generation(std::move(generation)));
    }
  }

  const Json* sources = document.find("sources");
  if (sources != nullptr && sources->is_array()) {
    for (const auto& item : sources->as_array()) {
      Source source;
      FEL_TRY_ASSIGN(const std::string id_text, item.require_string("id"));
      FEL_TRY_ASSIGN(source.id, SourceId::parse(id_text));
      FEL_TRY_ASSIGN(source.label, item.require_string("label"));
      FEL_TRY_ASSIGN(const std::string kind_text, item.require_string("kind"));
      const auto kind = source_kind_from_name(kind_text);
      if (!kind.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "unknown source kind", kind_text);
      }
      source.kind = kind.value();
      std::uint64_t authority = 0;
      FEL_TRY_ASSIGN(authority, item.require_u64("authority"));
      source.authority = AuthorityRank{authority};
      FEL_TRY_ASSIGN(const std::string provenance_text, item.require_string("provenance"));
      const auto provenance = provenance_class_from_name(provenance_text);
      if (!provenance.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "unknown provenance class", provenance_text);
      }
      source.provenance = provenance.value();
      FEL_TRY_ASSIGN(source.enabled, item.require_bool("enabled"));
      FEL_TRY(topology.add_source(std::move(source)));
    }
  }

  const Json* incarnations = document.find("incarnations");
  if (incarnations != nullptr && incarnations->is_array()) {
    for (const auto& item : incarnations->as_array()) {
      SourceIncarnation incarnation;
      FEL_TRY_ASSIGN(const std::string id_text, item.require_string("id"));
      FEL_TRY_ASSIGN(incarnation.id, IncarnationId::parse(id_text));
      FEL_TRY_ASSIGN(const std::string source_text, item.require_string("source"));
      FEL_TRY_ASSIGN(incarnation.source, SourceId::parse(source_text));
      std::uint64_t boot = 0;
      FEL_TRY_ASSIGN(boot, item.require_u64("boot_epoch"));
      incarnation.boot_epoch = EpochId{boot};
      FEL_TRY_ASSIGN(const std::string started_text, item.require_string("started_at"));
      FEL_TRY_ASSIGN(incarnation.started_at, parse_rfc3339(started_text));
      FEL_TRY_ASSIGN(incarnation.retired_at, parse_optional_time(item, "retired_at"));
      if (const Json* note = item.find("note"); note != nullptr && note->is_string()) {
        incarnation.note = note->as_string();
      }
      FEL_TRY(topology.add_incarnation(std::move(incarnation)));
    }
  }

  const Json* scopes = document.find("scopes");
  if (scopes != nullptr && scopes->is_array()) {
    for (const auto& item : scopes->as_array()) {
      Scope scope;
      FEL_TRY_ASSIGN(const std::string id_text, item.require_string("id"));
      FEL_TRY_ASSIGN(scope.id, ScopeId::parse(id_text));
      FEL_TRY_ASSIGN(scope.label, item.require_string("label"));
      FEL_TRY_ASSIGN(const std::string kind_text, item.require_string("kind"));
      const auto kind = scope_kind_from_name(kind_text);
      if (!kind.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "unknown scope kind", kind_text);
      }
      scope.kind = kind.value();
      FEL_TRY_ASSIGN(const std::string parent_text, item.require_string("parent"));
      if (parent_text != "-") {
        ScopeId parent;
        FEL_TRY_ASSIGN(parent, ScopeId::parse(parent_text));
        scope.parent = parent;
      }
      if (const Json* members = item.find("members"); members != nullptr && members->is_array()) {
        for (const auto& value : members->as_array()) {
          if (!value.is_string()) {
            return make_error(ErrorCode::SchemaViolation, "scope member must be a string");
          }
          ResourceId member;
          FEL_TRY_ASSIGN(member, ResourceId::parse(value.as_string()));
          scope.members.push_back(member);
        }
      }
      FEL_TRY(topology.add_scope(std::move(scope)));
    }
  }

  const Json* resources = document.find("resources");
  if (resources != nullptr && resources->is_array()) {
    for (const auto& item : resources->as_array()) {
      Resource resource;
      FEL_TRY_ASSIGN(const std::string id_text, item.require_string("id"));
      FEL_TRY_ASSIGN(resource.id, ResourceId::parse(id_text));
      FEL_TRY_ASSIGN(resource.label, item.require_string("label"));
      FEL_TRY_ASSIGN(const std::string kind_text, item.require_string("kind"));
      const auto kind = resource_kind_from_name(kind_text);
      if (!kind.has_value()) {
        return make_error(ErrorCode::InvalidArgument, "unknown resource kind", kind_text);
      }
      resource.kind = kind.value();
      FEL_TRY_ASSIGN(const std::string scope_text, item.require_string("scope"));
      FEL_TRY_ASSIGN(resource.scope, ScopeId::parse(scope_text));
      if (const Json* locality = item.find("locality"); locality != nullptr && locality->is_string()) {
        resource.locality = locality->as_string();
      }
      FEL_TRY(topology.add_resource(std::move(resource)));
    }
  }

  const Json* bindings = document.find("bindings");
  if (bindings != nullptr && bindings->is_array()) {
    for (const auto& item : bindings->as_array()) {
      GenerationBinding binding;
      FEL_TRY_ASSIGN(binding.resource, resource_id(item, "resource"));
      FEL_TRY_ASSIGN(const std::string generation_text, item.require_string("generation"));
      FEL_TRY_ASSIGN(binding.generation, GenerationId::parse(generation_text));
      FEL_TRY_ASSIGN(const std::string from_text, item.require_string("valid_from"));
      FEL_TRY_ASSIGN(binding.valid_from, parse_rfc3339(from_text));
      FEL_TRY_ASSIGN(binding.valid_to, parse_optional_time(item, "valid_to"));
      binding.id = BindingId::derive({binding.resource.to_string(), binding.generation.to_string(),
                                      std::to_string(binding.valid_from.nanos())});
      FEL_TRY(topology.add_binding(std::move(binding)));
    }
  }

  const Json* flows = document.find("flows");
  if (flows != nullptr && flows->is_array()) {
    for (const auto& item : flows->as_array()) {
      Flow flow;
      FEL_TRY_ASSIGN(const std::string id_text, item.require_string("id"));
      FEL_TRY_ASSIGN(flow.id, FlowId::parse(id_text));
      FEL_TRY_ASSIGN(flow.label, item.require_string("label"));
      FEL_TRY_ASSIGN(const std::string scope_text, item.require_string("scope"));
      FEL_TRY_ASSIGN(flow.scope, ScopeId::parse(scope_text));
      FEL_TRY_ASSIGN(const std::string generation_text, item.require_string("generation"));
      FEL_TRY_ASSIGN(flow.generation, GenerationId::parse(generation_text));
      if (const Json* hint = item.find("path_hint"); hint != nullptr && hint->is_array()) {
        for (const auto& value : hint->as_array()) {
          if (!value.is_string()) {
            return make_error(ErrorCode::SchemaViolation, "path hint entry must be a string");
          }
          ResourceId parsed;
          FEL_TRY_ASSIGN(parsed, ResourceId::parse(value.as_string()));
          flow.path_hint.push_back(parsed);
        }
      }
      FEL_TRY(topology.add_flow(std::move(flow)));
    }
  }

  const Json* paths = document.find("paths");
  if (paths != nullptr && paths->is_array()) {
    for (const auto& item : paths->as_array()) {
      Path path;
      FEL_TRY_ASSIGN(const std::string id_text, item.require_string("id"));
      FEL_TRY_ASSIGN(path.id, PathId::parse(id_text));
      FEL_TRY_ASSIGN(path.label, item.require_string("label"));
      FEL_TRY_ASSIGN(const std::string scope_text, item.require_string("scope"));
      FEL_TRY_ASSIGN(path.scope, ScopeId::parse(scope_text));
      FEL_TRY_ASSIGN(const std::string generation_text, item.require_string("generation"));
      FEL_TRY_ASSIGN(path.generation, GenerationId::parse(generation_text));
      if (const Json* hops = item.find("hops"); hops != nullptr && hops->is_array()) {
        for (const auto& value : hops->as_array()) {
          if (!value.is_string()) {
            return make_error(ErrorCode::SchemaViolation, "hop entry must be a string");
          }
          ResourceId parsed;
          FEL_TRY_ASSIGN(parsed, ResourceId::parse(value.as_string()));
          path.hops.push_back(parsed);
        }
      }
      FEL_TRY(topology.add_path(std::move(path)));
    }
  }

  const Json* reservations = document.find("reservations");
  if (reservations != nullptr && reservations->is_array()) {
    for (const auto& item : reservations->as_array()) {
      Reservation reservation;
      FEL_TRY_ASSIGN(const std::string id_text, item.require_string("id"));
      FEL_TRY_ASSIGN(reservation.id, ReservationId::parse(id_text));
      FEL_TRY_ASSIGN(reservation.label, item.require_string("label"));
      FEL_TRY_ASSIGN(const std::string scope_text, item.require_string("scope"));
      FEL_TRY_ASSIGN(reservation.scope, ScopeId::parse(scope_text));
      FEL_TRY_ASSIGN(const std::string generation_text, item.require_string("generation"));
      FEL_TRY_ASSIGN(reservation.generation, GenerationId::parse(generation_text));
      FEL_TRY_ASSIGN(const std::string resource_text, item.require_string("resource"));
      if (resource_text != "-") {
        ResourceId parsed;
        FEL_TRY_ASSIGN(parsed, ResourceId::parse(resource_text));
        reservation.resource = parsed;
      }
      FEL_TRY_ASSIGN(reservation.reserved_capacity_nanos, item.require_u64("reserved_capacity_nanos"));
      FEL_TRY_ASSIGN(const std::string from_text, item.require_string("valid_from"));
      FEL_TRY_ASSIGN(reservation.valid_from, parse_rfc3339(from_text));
      FEL_TRY_ASSIGN(reservation.valid_to, parse_optional_time(item, "valid_to"));
      FEL_TRY(topology.add_reservation(std::move(reservation)));
    }
  }

  FEL_TRY(topology.validate());
  return topology;
}

Result<FabricTopology> parse_topology_document(std::string_view text) {
  auto parsed = Json::parse(text);
  if (!parsed.has_value()) {
    return make_error(ErrorCode::MalformedJson, "topology document is not valid JSON",
                      parsed.error().message);
  }
  return topology_from_json(parsed.value());
}

}  // namespace detail
}  // namespace fel
