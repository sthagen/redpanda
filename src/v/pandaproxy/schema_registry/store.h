/*
 * Copyright 2021 Vectorized, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#pragma once

#include "outcome.h"
#include "pandaproxy/schema_registry/error.h"
#include "pandaproxy/schema_registry/types.h"

#include <absl/container/btree_map.h>
#include <absl/container/node_hash_map.h>

namespace pandaproxy::schema_registry {

class store {
public:
    struct insert_result {
        schema_version version;
        schema_id id;
        bool inserted;
    };
    ///\brief Insert a schema for a given subject.
    ///
    /// If the schema is not registered, register it.
    /// If the subject does not have this schema at any version, register a new
    /// version.
    ///
    /// return the schema_version and schema_id, and whether it's new.
    insert_result insert(subject sub, schema_definition def, schema_type type) {
        auto id = insert_schema(std::move(def), type).id;
        auto [version, inserted] = insert_subject(std::move(sub), id);
        return {version, id, inserted};
    }

    ///\brief Update or insert a schema with the given id, and register it with
    /// the subject for the given version.
    ///
    /// return true if a new version was inserted, false if updated.
    bool upsert(
      subject sub,
      schema_definition def,
      schema_type type,
      schema_id id,
      schema_version version,
      is_deleted deleted) {
        upsert_schema(id, std::move(def), type);
        return upsert_subject(std::move(sub), version, id, deleted);
    }

    ///\brief Return a schema by id.
    result<schema> get_schema(const schema_id& id) const {
        auto it = _schemas.find(id);
        if (it == _schemas.end()) {
            return error_code::schema_id_not_found;
        }
        return {it->first, it->second.type, it->second.definition};
    }

    ///\brief Return a schema by subject and version.
    result<subject_schema> get_subject_schema(
      const subject& sub,
      schema_version version,
      include_deleted inc_del) const {
        auto sub_it = _subjects.find(sub);
        if (sub_it == _subjects.end()) {
            return error_code::subject_not_found;
        }

        if (sub_it->second.deleted && !inc_del) {
            return error_code::subject_not_found;
        }

        const auto& versions = sub_it->second.versions;
        auto v_it = std::lower_bound(
          versions.begin(),
          versions.end(),
          version,
          [](const subject_version_id& lhs, schema_version rhs) {
              return lhs.version < rhs;
          });
        if (v_it == versions.end() || v_it->version != version) {
            return error_code::subject_version_not_found;
        }

        auto s = get_schema(v_it->id);
        if (!s) {
            return s.as_failure();
        }

        return subject_schema{
          .sub = sub,
          .version = v_it->version,
          .id = v_it->id,
          .type = s.value().type,
          .definition = std::move(s).value().definition,
          .deleted = v_it->deleted};
    }

    ///\brief Return a list of subjects.
    std::vector<subject> get_subjects(include_deleted inc_del) const {
        std::vector<subject> res;
        res.reserve(_subjects.size());
        for (const auto& sub : _subjects) {
            if (inc_del || !sub.second.deleted) {
                res.push_back(sub.first);
            }
        }
        return res;
    }

    ///\brief Return a list of versions and associated schema_id.
    result<std::vector<schema_version>>
    get_versions(const subject& sub, include_deleted inc_del) const {
        auto sub_it = _subjects.find(sub);
        if (sub_it == _subjects.end()) {
            return error_code::subject_not_found;
        }

        if (sub_it->second.deleted && !inc_del) {
            return error_code::subject_not_found;
        }

        const auto& versions = sub_it->second.versions;
        std::vector<schema_version> res;
        res.reserve(versions.size());
        for (const auto& ver : versions) {
            if (inc_del || !(ver.deleted || sub_it->second.deleted)) {
                res.push_back(ver.version);
            }
        }
        return res;
    }

    ///\brief Delete a subject.
    result<std::vector<schema_version>>
    delete_subject(const subject& sub, permanent_delete permanent) {
        auto sub_it = _subjects.find(sub);
        if (sub_it == _subjects.end()) {
            return error_code::subject_not_found;
        }

        if (permanent && !sub_it->second.deleted) {
            return error_code::subject_not_deleted;
        }

        if (!permanent && sub_it->second.deleted) {
            return error_code::subject_soft_deleted;
        }

        sub_it->second.deleted = is_deleted::yes;

        const auto& versions = sub_it->second.versions;
        std::vector<schema_version> res;
        res.reserve(versions.size());
        std::transform(
          versions.begin(),
          versions.end(),
          std::back_inserter(res),
          [](const auto& v) { return v.version; });

        if (permanent) {
            _subjects.erase(sub_it);
        }

        return res;
    }

    ///\brief Delete a subject version
    result<bool> delete_subject_version(
      const subject& sub,
      schema_version version,
      permanent_delete permanent,
      include_deleted inc_del) {
        auto sub_it = _subjects.find(sub);
        if (sub_it == _subjects.end()) {
            return error_code::subject_not_found;
        }

        if (sub_it->second.deleted && !inc_del) {
            return error_code::subject_not_found;
        }

        auto& versions = sub_it->second.versions;
        auto v_it = std::lower_bound(
          versions.begin(),
          versions.end(),
          version,
          [](const subject_version_id& lhs, schema_version rhs) {
              return lhs.version < rhs;
          });
        if (v_it == versions.end() || v_it->version != version) {
            return error_code::subject_version_not_found;
        }

        if (!v_it->deleted && permanent && !inc_del) {
            return error_code::subject_version_not_deleted;
        }

        if (v_it->deleted && !permanent && !inc_del) {
            return error_code::subject_version_soft_deleted;
        }

        if (permanent) {
            versions.erase(v_it);
            return true;
        }
        return std::exchange(v_it->deleted, is_deleted::yes) != is_deleted::yes;
    }

    ///\brief Get the global compatibility level.
    result<compatibility_level> get_compatibility() const {
        return _compatibility;
    }

    ///\brief Get the compatibility level for a subject, or fallback to global.
    result<compatibility_level> get_compatibility(const subject& sub) const {
        auto sub_it = _subjects.find(sub);
        if (sub_it == _subjects.end()) {
            return error_code::subject_not_found;
        }

        if (sub_it->second.deleted) {
            return error_code::subject_not_found;
        }

        return sub_it->second.compatibility.value_or(_compatibility);
    }

    ///\brief Set the global compatibility level.
    result<bool> set_compatibility(compatibility_level compatibility) {
        return std::exchange(_compatibility, compatibility) != compatibility;
    }

    ///\brief Set the compatibility level for a subject.
    result<bool>
    set_compatibility(const subject& sub, compatibility_level compatibility) {
        auto sub_it = _subjects.find(sub);
        if (sub_it == _subjects.end()) {
            return error_code::subject_not_found;
        }

        if (sub_it->second.deleted) {
            return error_code::subject_not_found;
        }

        // TODO(Ben): Check needs to be made here?
        return std::exchange(sub_it->second.compatibility, compatibility)
               != compatibility;
    }

    ///\brief Clear the compatibility level for a subject.
    result<bool> clear_compatibility(const subject& sub) {
        auto sub_it = _subjects.find(sub);
        if (sub_it == _subjects.end()) {
            return error_code::subject_not_found;
        }
        return std::exchange(sub_it->second.compatibility, std::nullopt)
               != std::nullopt;
    }

    ///\brief Check if the provided schema is compatible with the subject and
    /// version, according the the current compatibility.
    ///
    /// If the compatibility level is transitive, then all versions are checked,
    /// otherwise checks are against the version provided and newer.
    result<bool> is_compatible(
      const subject& sub,
      schema_version version,
      const schema_definition& new_schema,
      schema_type new_schema_type);

private:
    struct insert_schema_result {
        schema_id id;
        bool inserted;
    };
    insert_schema_result
    insert_schema(schema_definition def, schema_type type) {
        const auto s_it = std::find_if(
          _schemas.begin(), _schemas.end(), [&](const auto& s) {
              const auto& entry = s.second;
              return type == entry.type && def == entry.definition;
          });
        if (s_it != _schemas.end()) {
            return {s_it->first, false};
        }

        const auto id = _schemas.empty() ? schema_id{1}
                                         : std::prev(_schemas.end())->first + 1;
        auto [_, inserted] = _schemas.try_emplace(id, type, std::move(def));
        return {id, inserted};
    }

    bool upsert_schema(schema_id id, schema_definition def, schema_type type) {
        return _schemas.insert_or_assign(id, schema_entry(type, std::move(def)))
          .second;
    }

    struct insert_subject_result {
        schema_version version;
        bool inserted;
    };
    insert_subject_result insert_subject(subject sub, schema_id id) {
        auto& subject_entry = _subjects[std::move(sub)];
        subject_entry.deleted = is_deleted::no;
        auto& versions = subject_entry.versions;
        const auto v_it = std::find_if(
          versions.begin(), versions.end(), [id](auto v) {
              return v.id == id;
          });
        if (v_it != versions.cend()) {
            auto inserted = std::exchange(v_it->deleted, is_deleted::no);
            return {v_it->version, bool(inserted)};
        }

        const auto version = versions.empty() ? schema_version{1}
                                              : versions.back().version + 1;
        versions.emplace_back(version, id, is_deleted::no);
        return {version, true};
    }

    bool upsert_subject(
      subject sub, schema_version version, schema_id id, is_deleted deleted) {
        auto& subject_entry = _subjects[std::move(sub)];
        // Inserting a version undeletes the subject
        subject_entry.deleted = is_deleted::no;
        auto& versions = subject_entry.versions;
        const auto v_it = std::lower_bound(
          versions.begin(),
          versions.end(),
          version,
          [](const subject_version_id& lhs, schema_version rhs) {
              return lhs.version < rhs;
          });
        if (v_it != versions.end() && v_it->version == version) {
            *v_it = subject_version_id(version, id, deleted);
            return false;
        }
        versions.insert(v_it, subject_version_id(version, id, deleted));
        return true;
    }

    struct schema_entry {
        schema_entry(schema_type type, schema_definition definition)
          : type{type}
          , definition{std::move(definition)} {}

        schema_type type;
        schema_definition definition;
    };

    struct subject_entry {
        std::optional<compatibility_level> compatibility;
        std::vector<subject_version_id> versions;
        is_deleted deleted{false};
    };

    absl::btree_map<schema_id, schema_entry> _schemas;
    absl::node_hash_map<subject, subject_entry> _subjects;
    compatibility_level _compatibility{compatibility_level::none};
};

} // namespace pandaproxy::schema_registry
