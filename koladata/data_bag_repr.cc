// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include "koladata/data_bag_repr.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "koladata/data_bag.h"
#include "koladata/internal/data_bag.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/object_id.h"
#include "koladata/internal/schema_utils.h"
#include "koladata/internal/triples.h"
#include "arolla/util/text.h"
#include "arolla/util/status_macros_backport.h"

namespace koladata {
namespace {

using ::koladata::internal::DataBagContent;
using ::koladata::internal::DataItem;
using ::koladata::internal::ObjectId;
using ::koladata::internal::debug::AttrTriple;
using ::koladata::internal::debug::DictItemTriple;
using ::koladata::internal::debug::Triples;

using AttrMap =
    absl::flat_hash_map<DataItem, DataItem, DataItem::Hash, DataItem::Eq>;

constexpr int kTwoSpaceIndentation = 2;

constexpr absl::string_view kDictValuesNameReplacement = "<dict value>";
constexpr absl::string_view kListItemsNameReplacement = "<list items>";
constexpr absl::string_view kSchemaNameReplacement = "<object schemas>";

struct DataBagFormatOption {
  int indentation = 0;
  std::optional<int> fallback_index;
};

// Builds the schema attr triples into a map.
absl::flat_hash_map<ObjectId, AttrMap> BuildSchemaAttrMap(
    absl::Span<const DictItemTriple> schemas) {
  absl::flat_hash_map<ObjectId, AttrMap> result;
  for (const DictItemTriple& triple : schemas) {
    if (!triple.object.IsSchema()) {
      continue;
    }
    auto [it, _] = result.try_emplace(triple.object, AttrMap());
    it->second.emplace(triple.key, triple.value);
  }
  return result;
}

// Returns the string representation of the schema. The schema is
// recursively expanded if it's nested list or dict schema.
std::string SchemaToStr(
    const DataItem& schema_item,
    const absl::flat_hash_map<ObjectId, AttrMap>& triple_map,
    int64_t depth = 5);

// Returns the string representation of the schema attribute value. Returns
// empty string if the attribute is not found.
std::string AttrValueToStr(
    absl::string_view attr, const AttrMap& attr_map,
    const absl::flat_hash_map<ObjectId, AttrMap>& triple_map, int64_t depth) {
  auto it = attr_map.find(DataItem(arolla::Text(attr)));
  if (it == attr_map.end()) {
    return "";
  }
  return SchemaToStr(it->second, triple_map, depth - 1);
}

std::string SchemaToStr(
    const DataItem& schema_item,
    const absl::flat_hash_map<ObjectId, AttrMap>& triple_map, int64_t depth) {
  if (!schema_item.holds_value<ObjectId>()) {
    return absl::StrCat(schema_item);
  }
  DCHECK_GE(depth, 0);
  if (depth == 0) {
    return "...";
  }
  const ObjectId& schema = schema_item.value<ObjectId>();
  auto it = triple_map.find(schema);
  if (it == triple_map.end()) {
    return "";
  }
  auto list_schema_str = AttrValueToStr(schema::kListItemsSchemaAttr,
                                        it->second, triple_map, depth);

  if (!list_schema_str.empty()) {
    return absl::StrCat("list<", list_schema_str, ">");
  }
  auto key_schema_str =
                   AttrValueToStr(schema::kDictKeysSchemaAttr, it->second,
                                  triple_map, depth);
  auto value_schema_str =
                   AttrValueToStr(schema::kDictValuesSchemaAttr, it->second,
                                  triple_map, depth);
  if (!key_schema_str.empty() && !value_schema_str.empty()) {
    return absl::StrCat(internal::DataItemRepr(schema_item), "[dict<",
                        key_schema_str, ", ", value_schema_str, ">]");
  }
  return DataItemRepr(schema_item);
}

// Converts internal attribute names (such as '__schema__', '__items__',
// '__keys__', '__values__') to more user readable names.
std::string AttributeRepr(const absl::string_view attribute) {
  if (attribute == schema::kSchemaAttr) {
    return "get_obj_schema()";
  } else if (attribute == schema::kListItemsSchemaAttr) {
    return "get_item_schema()";
  } else if (attribute == schema::kDictKeysSchemaAttr) {
    return "get_key_schema()";
  } else if (attribute == schema::kDictValuesSchemaAttr) {
    return "get_value_schema()";
  } else {
    return std::string(attribute);
  }
}

class ContentsReprBuilder {
 public:
  explicit ContentsReprBuilder(const DataBagPtr& db, int64_t triple_limit)
      : db_(db), triple_count_(0), triple_limit_(triple_limit) {}

  absl::StatusOr<std::string> Build(bool show_data, bool show_schema) && {
    if (triple_limit_ <= 0) {
      return absl::InvalidArgumentError(
          "triple_limit must be a positive integer");
    }

    // Extract necessary triples.
    ASSIGN_OR_RETURN(DataBagContent content, db_->GetImpl().ExtractContent());
    Triples main_triples(content);
    FlattenFallbackFinder fallback_finder(*db_);
    auto fallbacks = fallback_finder.GetFlattenFallbacks();
    std::vector<Triples> fallback_triples;
    fallback_triples.reserve(fallbacks.size());
    for (const internal::DataBagImpl* const fallback : fallbacks) {
      ASSIGN_OR_RETURN(DataBagContent fallback_content,
                        fallback->ExtractContent());
      fallback_triples.push_back(Triples(fallback_content));
    }

    if (show_data) {
      res_ = absl::StrCat("DataBag ", GetBagIdRepr(db_), ":\n");
      // Triples in the main DataBag.
      AddDataTriples(main_triples);
      if (triple_count_ >= triple_limit_) {
        Etcetera();
        return std::move(res_);
      }

      // Triples in the fallbacks.
      for (const auto& triples : fallback_triples) {
        AddDataTriples(triples);
        if (triple_count_ >= triple_limit_) {
          Etcetera();
          return std::move(res_);
        }
      }
    }

    if (show_schema) {
      if (show_data) {
        absl::StrAppend(&res_, "\nSchemaBag:\n");
      } else {
        res_ = absl::StrCat("SchemaBag ", GetBagIdRepr(db_), ":\n");
      }
      // Schema triples in the main DataBag.
      RETURN_IF_ERROR(AddSchemaTriples(main_triples));
      if (triple_count_ >= triple_limit_) {
          Etcetera();
          return std::move(res_);
      }

      // Schema triples in the fallbacks.
      for (const auto& triples : fallback_triples) {
        RETURN_IF_ERROR(AddSchemaTriples(triples));
        if (triple_count_ >= triple_limit_) {
          Etcetera();
          return std::move(res_);
        }
      }
    }

    return std::move(res_);
  }

 private:
  void Etcetera() {
    absl::StrAppend(&res_, "...\n\n",
                    absl::StrFormat("Showing only the first %d triples. Use "
                                    "'triple_limit' parameter of "
                                    "'db.contents_repr()' to adjust this\n",
                                    triple_count_));
  }

  void AddAttributeTriples(const Triples& triples) {
    for (const AttrTriple& attr : triples.attributes()) {
      if (seen_triples_.contains({attr.object, attr.attribute})) {
        continue;
      }
      seen_triples_.insert({attr.object, attr.attribute});
      absl::StrAppend(&res_,
                      absl::StrFormat("%s.%s => %s\n", ObjectIdStr(attr.object),
                                      AttributeRepr(attr.attribute),
                                      internal::DataItemRepr(
                                          attr.value, {.strip_quotes = true})));
      if (++triple_count_ >= triple_limit_) {
        return;
      }
    }
  }

  void AddListTriples(const Triples& triples) {
    for (const auto& [list_id, values] : triples.lists()) {
      if (seen_triples_.contains({list_id, "[:]"})) {
        continue;
      }
      seen_triples_.insert({list_id, "[:]"});
      absl::StrAppend(
          &res_, absl::StrFormat(
                     "%s[:] => [%s]\n", ObjectIdStr(list_id),
                     absl::StrJoin(
                         values.begin(), values.end(), ", ",
                         [](std::string* out, const internal::DataItem& item) {
                           out->append(internal::DataItemRepr(item));
                         })));
      if (++triple_count_ >= triple_limit_) {
        return;
      }
    }
  }

  void AddDictTriples(const Triples& triples) {
    for (const DictItemTriple& dict : triples.dicts()) {
      if (dict.object.IsDict()) {
        const auto& attr_str = DataItemRepr(dict.key);
        if (seen_triples_.contains({dict.object, attr_str})) {
          continue;
        }
        seen_triples_.insert({dict.object, attr_str});
        absl::StrAppend(
            &res_,
            absl::StrFormat("%s[%s] => %s\n", ObjectIdStr(dict.object),
                            attr_str, internal::DataItemRepr(dict.value)));
        if (++triple_count_ >= triple_limit_) {
          return;
        }
      }
    }
  }

  void AddDataTriples(const Triples& triples) {
    AddAttributeTriples(triples);
    if (triple_count_ >= triple_limit_) {
      return;
    }
    AddListTriples(triples);
    if (triple_count_ >= triple_limit_) {
      return;
    }
    AddDictTriples(triples);
    if (triple_count_ >= triple_limit_) {
      return;
    }
  }

  absl::Status AddSchemaTriples(const Triples& triples) {
    absl::flat_hash_map<ObjectId, AttrMap> schema_triple_map =
        BuildSchemaAttrMap(triples.dicts());
    for (const DictItemTriple& dict : triples.dicts()) {
      if (dict.object.IsSchema()) {
        const auto& attr_str =
            std::string(dict.key.value<arolla::Text>().view());
        if (seen_triples_.contains({dict.object, attr_str})) {
          continue;
        }
        seen_triples_.insert({dict.object, attr_str});
        auto value_str = SchemaToStr(dict.value, schema_triple_map);
        if (value_str.empty())
          continue;
        absl::StrAppend(
            &res_, absl::StrFormat("%s.%s => %s\n", ObjectIdStr(dict.object),
                                   AttributeRepr(attr_str), value_str));
        if (++triple_count_ >= triple_limit_) {
          return absl::OkStatus();
        }
      }
    }
    return absl::OkStatus();
  }

  const DataBagPtr db_;
  std::string res_;
  int64_t triple_count_;
  int64_t triple_limit_;
  absl::flat_hash_set<std::pair<ObjectId, std::string>> seen_triples_;
};

template <typename Map>
void UpdateCountMap(const typename Map::key_type& val, Map& count_dict) {
  static_assert(std::is_same<typename Map::mapped_type, int64_t>::value,
                "mapped_type must be int64_t");
  auto [it, inserted] = count_dict.emplace(val, 1);
  if (!inserted) {
    ++it->second;
  }
}

}  // namespace

absl::StatusOr<std::string> DataBagToStr(const DataBagPtr& db,
                                         int64_t triple_limit) {
  ContentsReprBuilder builder(db, triple_limit);
  return std::move(builder).Build(/*show_data=*/true, /*show_schema=*/true);
}

absl::StatusOr<std::string> DataOnlyBagToStr(const DataBagPtr& db,
                                         int64_t triple_limit) {
  ContentsReprBuilder builder(db, triple_limit);
  return std::move(builder).Build(/*show_data=*/true, /*show_schema=*/false);
}

absl::StatusOr<std::string> SchemaOnlyBagToStr(const DataBagPtr& db,
                                         int64_t triple_limit) {
  ContentsReprBuilder builder(db, triple_limit);
  return std::move(builder).Build(/*show_data=*/false, /*show_schema=*/true);
}

absl::StatusOr<std::string> DataBagStatistics(const DataBagPtr& db,
                                              size_t top_attr_limit) {
  ASSIGN_OR_RETURN(internal::DataBagStatistics stats,
                   db->GetImpl().GetStatistics());

  std::vector<std::pair<int, absl::string_view>> top_attrs;
  top_attrs.reserve(stats.attr_values_sizes.size());
  for (const auto& [attr, count] : stats.attr_values_sizes) {
    top_attrs.emplace_back(count, attr);
  }

  std::nth_element(
      top_attrs.begin(),
      top_attrs.begin() + std::min(top_attr_limit, top_attrs.size()),
      top_attrs.end(), std::greater<std::pair<size_t, absl::string_view>>());
  if (top_attrs.size() > top_attr_limit) {
    top_attrs.resize(top_attr_limit);
  }
  std::sort(top_attrs.begin(), top_attrs.end(),
            std::greater<std::pair<size_t, absl::string_view>>());
  size_t attr_value_count_sum = std::accumulate(
      top_attrs.begin(), top_attrs.end(), size_t{0},
      [](int64_t acc, const std::pair<int, absl::string_view>& attr_count) {
        return acc + attr_count.first;
      });

  std::string res = absl::StrFormat(
      R"(DataBag %s:
  %d Entities/Objects with %d values in %d attrs
  %d non empty Lists with %d items
  %d non empty Dicts with %d key/value entries
  %d schemas with %d values

Top attrs:
)",
      GetBagIdRepr(db), stats.entity_and_object_count, attr_value_count_sum,
      stats.attr_values_sizes.size(), stats.total_non_empty_lists,
      stats.total_items_in_lists, stats.total_non_empty_dicts,
      stats.total_items_in_dicts, stats.total_explicit_schemas,
      stats.total_explicit_schema_attrs);

  std::vector<std::string> top_attr_names;
  top_attr_names.reserve(top_attrs.size());
  for (const auto& [count, attr] : top_attrs) {
    top_attr_names.push_back(absl::StrFormat("  %s: %d values", attr, count));
  }
  res.append(absl::StrJoin(top_attr_names, "\n"));

  return res;
}

}  // namespace koladata
