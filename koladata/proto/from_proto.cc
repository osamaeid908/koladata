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
#include "koladata/proto/from_proto.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/base/nullability.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "koladata/adoption_utils.h"
#include "koladata/casting.h"
#include "koladata/data_bag.h"
#include "koladata/data_slice.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/data_slice.h"
#include "koladata/internal/dtype.h"
#include "koladata/internal/schema_utils.h"
#include "koladata/object_factories.h"
#include "koladata/operators/core.h"
#include "koladata/uuid_utils.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "arolla/dense_array/dense_array.h"
#include "arolla/util/bytes.h"
#include "arolla/util/text.h"
#include "arolla/util/unit.h"
#include "arolla/util/status_macros_backport.h"

using ::google::protobuf::Descriptor;
using ::google::protobuf::DescriptorPool;
using ::google::protobuf::FieldDescriptor;
using ::google::protobuf::Message;
using ::google::protobuf::Reflection;

namespace koladata {
namespace {

// Extension specifier parsing.

struct ExtensionMap {
  // Extension fields that should be converted in this message.
  //
  // Key: "(" + field->full_name() + ")"
  absl::flat_hash_map<std::string, absl::Nonnull<const FieldDescriptor*>>
      extension_fields;

  // Extension maps for sub-messages of this message.
  //
  // Key: field name of submessage field for normal fields, or
  // "(" + field->full_name() + ")" for extension fields.
  absl::flat_hash_map<std::string, std::unique_ptr<ExtensionMap>>
      submessage_extension_maps;
};

absl::Status ParseExtensionInto(const google::protobuf::DescriptorPool& pool,
                                absl::string_view extension_specifier,
                                ExtensionMap& root_extension_map) {
  absl::Nonnull<ExtensionMap*> extension_map = &root_extension_map;

  auto get_or_create_sub_map = [](ExtensionMap& map,
                                  absl::string_view ext_name) {
    std::unique_ptr<ExtensionMap>& sub_map =
        map.submessage_extension_maps[ext_name];
    if (sub_map == nullptr) {
      sub_map = std::make_unique<ExtensionMap>();
    }
    return sub_map.get();
  };

  // When we encounter a '(', set `in_ext_path`, and accumulate path pieces
  // into `ext_path_pieces` until we encounter a ')', then clear `in_ext_path`.
  bool in_ext_path = false;
  std::vector<absl::string_view> ext_path_pieces;

  const std::vector<absl::string_view> pieces =
      absl::StrSplit(extension_specifier, absl::ByChar('.'));
  for (size_t i_piece = 0; i_piece < pieces.size(); ++i_piece) {
    const auto& piece = pieces[i_piece];
    if (piece.starts_with('(')) {
      if (!ext_path_pieces.empty()) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "invalid extension path (unexpected opening parenthesis): \"%s\"",
            extension_specifier));
      }
      in_ext_path = true;
    }
    if (piece.ends_with(')')) {
      if (!in_ext_path) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "invalid extension path (unexpected closing parenthesis): \"%s\"",
            extension_specifier));
      }
      ext_path_pieces.push_back(piece);
      in_ext_path = false;

      // Note: `ext_name` includes starting and ending parens.
      auto ext_name = absl::StrJoin(ext_path_pieces, ".");
      DCHECK_GE(ext_name.size(), 2);
      auto ext_full_path =
          absl::string_view(&ext_name.data()[1], ext_name.size() - 2);
      ext_path_pieces.clear();

      const auto* ext_field_descriptor =
          pool.FindExtensionByName(ext_full_path);
      if (ext_field_descriptor == nullptr) {
        return absl::InvalidArgumentError(
            absl::StrFormat("extension not found: \"%s\"", ext_full_path));
      }

      if (i_piece == pieces.size() - 1) {
        extension_map->extension_fields[ext_name] = ext_field_descriptor;
      } else {
        extension_map = get_or_create_sub_map(*extension_map, ext_name);
      }
      continue;
    }

    if (in_ext_path) {
      ext_path_pieces.push_back(piece);
    } else {
      if (i_piece == pieces.size() - 1) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "invalid extension path (trailing non-extension field): \"%s\"",
            extension_specifier));
      }
      extension_map = get_or_create_sub_map(*extension_map, piece);
    }
  }

  if (in_ext_path) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "invalid extension path (missing closing parenthesis): \"%s\"",
        extension_specifier));
  }

  return absl::OkStatus();
}

absl::StatusOr<ExtensionMap> ParseExtensions(
    absl::Span<const absl::string_view> extensions,
    const DescriptorPool& pool) {
  ExtensionMap result;
  for (const auto& extension_path : extensions) {
    RETURN_IF_ERROR(ParseExtensionInto(pool, extension_path, result));
  }
  return result;
}

const ExtensionMap* GetChildExtensionMap(const ExtensionMap* extension_map,
                                         absl::string_view field_name) {
  if (extension_map != nullptr) {
    const auto& sub_maps = extension_map->submessage_extension_maps;
    if (auto lookup = sub_maps.find(field_name); lookup != sub_maps.end()) {
      return lookup->second.get();
    }
  }
  return nullptr;
}

// Shape / Schema / ItemId Helpers.

absl::StatusOr<std::optional<DataSlice>> GetChildAttrSchema(
    const std::optional<DataSlice>& schema, absl::string_view attr_name) {
  if (!schema.has_value()) {
    return std::nullopt;
  }
  if (schema->item() == schema::kObject) {
    return schema;  // return OBJECT;
  }
  ASSIGN_OR_RETURN(auto child_schema, schema->GetAttr(attr_name));
  return child_schema;
}

absl::StatusOr<std::optional<DataSlice>> GetMessageListItemsSchema(
    const std::optional<DataSlice>& schema) {
  return GetChildAttrSchema(schema, schema::kListItemsSchemaAttr);
}

absl::StatusOr<std::optional<DataSlice>> GetPrimitiveListItemsSchema(
    const std::optional<DataSlice>& schema) {
  if (!schema.has_value() || schema->item() == schema::kObject) {
    return std::nullopt;
  }
  ASSIGN_OR_RETURN(auto child_schema,
                   schema->GetAttr(schema::kListItemsSchemaAttr));
  return child_schema;
}

class Shape2DBuilder {
 public:
  explicit Shape2DBuilder(int64_t num_groups) : edge_builder_(num_groups + 1) {
    edge_builder_.Add(0, 0);
    ++i_next_;
  }

  void Add(int64_t group_size) {
    last_split_ += group_size;
    edge_builder_.Add(i_next_, last_split_);
    ++i_next_;
  }

  absl::StatusOr<DataSlice::JaggedShape> Build() && {
    auto edge_array = std::move(edge_builder_).Build();
    ASSIGN_OR_RETURN(auto edge0,
                     DataSlice::JaggedShape::Edge::FromUniformGroups(
                         1, edge_array.size() - 1));
    ASSIGN_OR_RETURN(auto edge1, DataSlice::JaggedShape::Edge::FromSplitPoints(
                                     std::move(edge_array)));
    return DataSlice::JaggedShape::FromEdges(
        {std::move(edge0), std::move(edge1)});
  }

 private:
  arolla::DenseArrayBuilder<int64_t> edge_builder_;
  int64_t i_next_ = 0;
  int64_t last_split_ = 0;
};

absl::StatusOr<DataSlice> CreateBareProtoUuSchema(
    const DataBagPtr& db, const Descriptor& message_descriptor) {
  return CreateUuSchema(
      db, absl::StrFormat("__from_proto_%s__", message_descriptor.full_name()),
      {}, {});
}

constexpr static absl::string_view kChildItemIdSeed = "__from_proto_child__";

absl::StatusOr<DataSlice> MakeTextItem(absl::string_view text) {
  return DataSlice::Create(internal::DataItem(arolla::Text(text)),
                           internal::DataItem(schema::kText));
}

absl::StatusOr<std::optional<DataSlice>> MakeChildObjectAttrItemIds(
    const std::optional<DataSlice>& parent_itemid,
    absl::string_view attr_name) {
  if (!parent_itemid.has_value()) {
    return std::nullopt;
  }
  ASSIGN_OR_RETURN(auto attr_name_slice, MakeTextItem(attr_name));
  ASSIGN_OR_RETURN(
      auto child_itemids,
      CreateUuidFromFields(kChildItemIdSeed, {"parent", "attr_name"},
                           {*parent_itemid, std::move(attr_name_slice)}));
  return std::move(child_itemids);
}

absl::StatusOr<std::optional<DataSlice>> MakeChildListAttrItemIds(
    const std::optional<DataSlice>& parent_itemid,
    absl::string_view attr_name) {
  if (!parent_itemid.has_value()) {
    return std::nullopt;
  }
  ASSIGN_OR_RETURN(auto attr_name_slice, MakeTextItem(attr_name));
  ASSIGN_OR_RETURN(
      auto child_itemids,
      CreateListUuidFromFields(kChildItemIdSeed, {"parent", "attr_name"},
                               {*parent_itemid, std::move(attr_name_slice)}));
  return std::move(child_itemids);
}

absl::StatusOr<std::optional<DataSlice>> MakeChildDictAttrItemIds(
    const std::optional<DataSlice>& parent_itemid,
    absl::string_view attr_name) {
  if (!parent_itemid.has_value()) {
    return std::nullopt;
  }
  ASSIGN_OR_RETURN(auto attr_name_slice, MakeTextItem(attr_name));
  ASSIGN_OR_RETURN(
      auto child_itemids,
      CreateDictUuidFromFields(kChildItemIdSeed, {"parent", "attr_name"},
                               {*parent_itemid, std::move(attr_name_slice)}));
  return std::move(child_itemids);
}

// Returns a rank-1 DataSlice of ITEMID containing unique uuids for each index
// in the 2D shape `items_shape` (or nullopt if `parent_itemid` is nullopt).
// The result is flattened, but has the same total size as `items_shape`.
//
// If not nullopt, `parent_itemid` must be a rank-1 DataSlice of ITEMID with the
// same size as the first dimension of `items_shape`. Each child item id is a
// deterministic function of its parent item id (determined by the first dim)
// and its index into the second dim.
absl::StatusOr<std::optional<DataSlice>> MakeFlatChildIndexItemUuids(
    const std::optional<DataSlice>& parent_itemid,
    const DataSlice::JaggedShape& items_shape) {
  DCHECK_EQ(items_shape.rank(), 2);

  if (!parent_itemid.has_value()) {
    return std::nullopt;
  }

  // Ideally we'd call something like `M.array.agg_index` to make
  // `list_index`. This is tricky to do from C++, and the equivalent
  // code is only a few lines anyway.
  arolla::DenseArrayBuilder<int64_t> flat_index_builder(items_shape.size());
  const auto& splits = items_shape.edges()[1].edge_values().values;
  for (int64_t i_split = 0; i_split < splits.size() - 1; ++i_split) {
    for (int64_t i = splits[i_split]; i < splits[i_split + 1]; ++i) {
      flat_index_builder.Add(i, i - splits[i_split]);
    }
  }
  ASSIGN_OR_RETURN(
      auto index,
      DataSlice::Create(internal::DataSliceImpl::Create(
                            std::move(flat_index_builder).Build()),
                        items_shape, internal::DataItem(schema::kInt64)));
  ASSIGN_OR_RETURN(auto child_itemids,
                   CreateUuidFromFields(kChildItemIdSeed, {"parent", "index"},
                                        {*parent_itemid, std::move(index)}));
  ASSIGN_OR_RETURN(
      auto flat_child_itemids,
      std::move(child_itemids)
          .Reshape(items_shape.FlattenDims(0, items_shape.rank())));
  return std::move(flat_child_itemids);
}

// Forward declarations for recursion.
absl::StatusOr<std::optional<DataSlice>> FromProtoField(
    const absl::Nonnull<DataBagPtr>& db, absl::string_view attr_name,
    absl::string_view field_name, const FieldDescriptor& field_descriptor,
    absl::Span<const absl::Nonnull<const Message*>> parent_messages,
    const std::optional<DataSlice>& parent_itemid,
    const std::optional<DataSlice>& parent_schema,
    absl::Nullable<const ExtensionMap*> parent_extension_map,
    bool ignore_field_presence = false);
absl::StatusOr<DataSlice> FromProtoMessage(
    const absl::Nonnull<DataBagPtr>& db, const Descriptor& message_descriptor,
    absl::Span<const absl::Nonnull<const Message*>> messages,
    const std::optional<DataSlice>& itemid,
    const std::optional<DataSlice>& schema,
    absl::Nullable<const ExtensionMap*> extension_map);

// Returns a rank-1 DataSlice of Lists converted from a repeated message field
// on a vector of messages.
absl::StatusOr<std::optional<DataSlice>> ListFromProtoRepeatedMessageField(
    const absl::Nonnull<DataBagPtr>& db,
    absl::string_view attr_name,
    absl::string_view field_name,
    const FieldDescriptor& field_descriptor,
    absl::Span<const absl::Nonnull<const Message*>> parent_messages,
    const std::optional<DataSlice>& parent_itemid,
    const std::optional<DataSlice>& parent_schema,
    absl::Nullable<const ExtensionMap*> parent_extension_map) {
  bool is_empty = true;
  arolla::DenseArrayBuilder<arolla::Unit> lists_mask_builder(
      parent_messages.size());
  Shape2DBuilder shape_builder(parent_messages.size());
  std::vector<absl::Nonnull<const Message*>> flat_child_messages;
  for (int64_t i = 0; i < parent_messages.size(); ++i) {
    const auto& parent_message = *parent_messages[i];
    const auto& refl = *parent_message.GetReflection();
    const auto& field_ref =
        refl.GetRepeatedFieldRef<Message>(parent_message, &field_descriptor);
    shape_builder.Add(field_ref.size());
    for (const auto& child_message : field_ref) {
      flat_child_messages.push_back(&child_message);
    }
    if (!field_ref.empty()) {
      lists_mask_builder.Add(i, arolla::kUnit);
      is_empty = false;
    }
  }
  if (is_empty) {
    return std::nullopt;
  }

  ASSIGN_OR_RETURN(auto schema, GetChildAttrSchema(parent_schema, attr_name));
  ASSIGN_OR_RETURN(auto itemid,
                   MakeChildListAttrItemIds(parent_itemid, attr_name));
  const auto* extension_map =
      GetChildExtensionMap(parent_extension_map, field_name);

  ASSIGN_OR_RETURN(auto items_shape, std::move(shape_builder).Build());
  ASSIGN_OR_RETURN(auto items_schema, GetMessageListItemsSchema(schema));
  ASSIGN_OR_RETURN(auto flat_items_itemid,
                   MakeFlatChildIndexItemUuids(itemid, items_shape));
  ASSIGN_OR_RETURN(auto flat_items,
                   FromProtoMessage(db, *field_descriptor.message_type(),
                                    flat_child_messages, flat_items_itemid,
                                    items_schema, extension_map));
  ASSIGN_OR_RETURN(auto items,
                   std::move(flat_items).Reshape(std::move(items_shape)));
  ASSIGN_OR_RETURN(auto lists_mask,
                   DataSlice::Create(internal::DataSliceImpl::Create(
                                         std::move(lists_mask_builder).Build()),
                                     DataSlice::JaggedShape::FlatFromSize(
                                         parent_messages.size()),
                                     internal::DataItem(schema::kMask)));
  ASSIGN_OR_RETURN(auto lists,
                   CreateListLike(db, std::move(lists_mask), std::move(items),
                                  std::nullopt,
                                  std::nullopt, itemid));
  if (schema.has_value() && schema->item() == schema::kObject) {
    ASSIGN_OR_RETURN(lists, ToObject(std::move(lists)));
  }
  return lists;
}

// Returns a rank-1 DataSlice of Lists of primitives converted from a repeated
// primitive field on a vector of messages.
absl::StatusOr<std::optional<DataSlice>> ListFromProtoRepeatedPrimitiveField(
    const absl::Nonnull<DataBagPtr>& db, absl::string_view attr_name,
    absl::string_view field_name, const FieldDescriptor& field_descriptor,
    absl::Span<const absl::Nonnull<const Message*>> parent_messages,
    const std::optional<DataSlice>& parent_itemid,
    const std::optional<DataSlice>& parent_schema) {
  auto to_slice = [&]<typename T, typename U>()
      -> absl::StatusOr<std::optional<DataSlice>> {
    int64_t num_items = 0;
    for (int64_t i = 0; i < parent_messages.size(); ++i) {
      const auto& parent_message = *parent_messages[i];
      const auto* refl = parent_message.GetReflection();
      const auto& field_ref =
          refl->GetRepeatedFieldRef<U>(parent_message, &field_descriptor);
      num_items += field_ref.size();
    }
    if (num_items == 0) {
      return std::nullopt;
    }

    arolla::DenseArrayBuilder<T> flat_items_builder(num_items);
    arolla::DenseArrayBuilder<arolla::Unit> lists_mask_builder(
        parent_messages.size());
    Shape2DBuilder shape_builder(parent_messages.size());
    int64_t i_next_flat_item = 0;
    for (int64_t i = 0; i < parent_messages.size(); ++i) {
      const auto& parent_message = *parent_messages[i];
      const auto& refl = *parent_message.GetReflection();
      const auto& field_ref =
          refl.GetRepeatedFieldRef<U>(parent_message, &field_descriptor);
      shape_builder.Add(field_ref.size());
      for (const U& item_value : field_ref) {
        flat_items_builder.Add(i_next_flat_item, item_value);
        ++i_next_flat_item;
      }
      if (!field_ref.empty()) {
        lists_mask_builder.Add(i, arolla::kUnit);
      }
    }

    ASSIGN_OR_RETURN(auto schema, GetChildAttrSchema(parent_schema, attr_name));
    ASSIGN_OR_RETURN(auto itemid,
                     MakeChildListAttrItemIds(parent_itemid, attr_name));

    ASSIGN_OR_RETURN(auto items_shape, std::move(shape_builder).Build());
    ASSIGN_OR_RETURN(
        auto items,
        DataSlice::Create(internal::DataSliceImpl::Create(
                              std::move(flat_items_builder).Build()),
                          std::move(items_shape),
                          internal::DataItem(schema::GetDType<T>())));
    ASSIGN_OR_RETURN(auto items_schema, GetPrimitiveListItemsSchema(schema));
    if (items_schema.has_value()) {
      // We could probably improve performance by using the correct backing
      // DenseArray type based on `schema` instead of casting afterward, but
      // that has a lot more cases to handle, and only has an effect if the
      // user provides an explicit schema that disagrees with the proto field
      // schemas, which should be rare.
      //
      // `validate_schema` is a no-op for primitives, so we disable it.
      ASSIGN_OR_RETURN(items, CastToExplicit(items, items_schema->item(),
                                             /*validate_schema=*/false));
    }

    ASSIGN_OR_RETURN(
        auto lists_mask,
        DataSlice::Create(
            internal::DataSliceImpl::Create(
                std::move(lists_mask_builder).Build()),
            DataSlice::JaggedShape::FlatFromSize(parent_messages.size()),
            internal::DataItem(schema::kMask)));
    ASSIGN_OR_RETURN(auto lists,
                     CreateListLike(db, std::move(lists_mask), std::move(items),
                                    std::nullopt, std::nullopt, itemid));
    if (schema.has_value() && schema->item() == schema::kObject) {
      ASSIGN_OR_RETURN(lists, ToObject(std::move(lists)));
    }
    return lists;
  };

  switch (field_descriptor.cpp_type()) {
    case FieldDescriptor::CPPTYPE_INT32:
      return to_slice.operator()<int32_t, int32_t>();
    case FieldDescriptor::CPPTYPE_INT64:
      return to_slice.operator()<int64_t, int64_t>();
    case FieldDescriptor::CPPTYPE_UINT32:
      return to_slice.operator()<int64_t, uint32_t>();
    case FieldDescriptor::CPPTYPE_UINT64:
      return to_slice.operator()<int64_t, uint64_t>();
    case FieldDescriptor::CPPTYPE_DOUBLE:
      return to_slice.operator()<double, double>();
    case FieldDescriptor::CPPTYPE_FLOAT:
      return to_slice.operator()<float, float>();
    case FieldDescriptor::CPPTYPE_BOOL:
      return to_slice.operator()<bool, bool>();
    case FieldDescriptor::CPPTYPE_ENUM:
      return to_slice.operator()<int32_t, int32_t>();
    case FieldDescriptor::CPPTYPE_STRING:
      if (field_descriptor.type() == FieldDescriptor::TYPE_STRING) {
        return to_slice.operator()<arolla::Text, std::string>();
      } else {  // TYPE_BYTES
        return to_slice.operator()<arolla::Bytes, std::string>();
      }
    default:
      return absl::InvalidArgumentError(absl::StrFormat(
          "unexpected proto field C++ type %d", field_descriptor.cpp_type()));
  }
}

// Returns a rank-1 DataSlice of Dicts converted from a proto map field on a
// vector of messages.
absl::StatusOr<std::optional<DataSlice>> DictFromProtoMapField(
    const absl::Nonnull<DataBagPtr>& db,
    absl::string_view attr_name,
    absl::string_view field_name,
    const FieldDescriptor& field_descriptor,
    absl::Span<const absl::Nonnull<const Message*>> parent_messages,
    const std::optional<DataSlice>& parent_itemid,
    const std::optional<DataSlice>& parent_schema,
    absl::Nullable<const ExtensionMap*> parent_extension_map) {
  bool is_empty = true;
  arolla::DenseArrayBuilder<arolla::Unit> dicts_mask_builder(
      parent_messages.size());
  Shape2DBuilder shape_builder(parent_messages.size());
  std::vector<absl::Nonnull<const Message*>> flat_item_messages;
  for (int64_t i = 0; i < parent_messages.size(); ++i) {
    const auto& parent_message = *parent_messages[i];
    const auto* refl = parent_message.GetReflection();
    const auto& field_ref =
        refl->GetRepeatedFieldRef<Message>(parent_message, &field_descriptor);
    shape_builder.Add(field_ref.size());
    for (const auto& item_message : field_ref) {
      flat_item_messages.push_back(&item_message);
    }
    if (!field_ref.empty()) {
      dicts_mask_builder.Add(i, arolla::kUnit);
      is_empty = false;
    }
  }
  if (is_empty) {
    return std::nullopt;
  }

  ASSIGN_OR_RETURN(auto schema, GetChildAttrSchema(parent_schema, attr_name));
  ASSIGN_OR_RETURN(auto itemid,
                   MakeChildDictAttrItemIds(parent_itemid, attr_name));
  const auto* extension_map =
      GetChildExtensionMap(parent_extension_map, field_name);

  ASSIGN_OR_RETURN(auto items_shape, std::move(shape_builder).Build());
  ASSIGN_OR_RETURN(auto flat_items_itemid,
                   MakeFlatChildIndexItemUuids(itemid, items_shape));
  const Descriptor& map_item_descriptor = *field_descriptor.message_type();
  // We set `ignore_field_presence` here because even though the `key` and
  // `value` fields of the map item message are marked as `optional` (report
  // having field presence via their field descriptors), the proto `Map` API
  // treats them as default-valued if they are unset, so we want them to be
  // converted to their default values in these DataSlices instead of being
  // missing.
  ASSIGN_OR_RETURN(
      std::optional<DataSlice> flat_keys,
      FromProtoField(db, schema::kDictKeysSchemaAttr, "keys",
                     *map_item_descriptor.map_key(), flat_item_messages,
                     flat_items_itemid, schema, extension_map,
                     /*ignore_field_presence=*/true));
  DCHECK(flat_keys.has_value());  // Implied by ignore_field_presence = true.
  ASSIGN_OR_RETURN(
      std::optional<DataSlice> flat_values,
      FromProtoField(db, schema::kDictValuesSchemaAttr, "values",
                     *map_item_descriptor.map_value(), flat_item_messages,
                     flat_items_itemid, schema, extension_map,
                    /*ignore_field_presence=*/true));
  ASSIGN_OR_RETURN(auto keys, std::move(flat_keys)->Reshape(items_shape));
  ASSIGN_OR_RETURN(auto values,
                   std::move(flat_values)->Reshape(std::move(items_shape)));
  ASSIGN_OR_RETURN(auto dicts_mask,
                   DataSlice::Create(internal::DataSliceImpl::Create(
                                         std::move(dicts_mask_builder).Build()),
                                     DataSlice::JaggedShape::FlatFromSize(
                                         parent_messages.size()),
                                     internal::DataItem(schema::kMask)));
  ASSIGN_OR_RETURN(
      auto dicts,
      CreateDictLike(db,
                     /*shape_and_mask_from=*/std::move(dicts_mask),
                     /*keys=*/std::move(keys),
                     /*values=*/std::move(values),
                     /*schema=*/std::nullopt,
                     /*key_schema=*/std::nullopt,
                     /*value_schema=*/std::nullopt,
                     /*itemid=*/itemid));
  if (schema.has_value() && schema->item() == schema::kObject) {
    ASSIGN_OR_RETURN(dicts, ToObject(std::move(dicts)));
  }
  return dicts;
}

// Returns a rank-1 DataSlice of objects or entities converted from a proto
// non-repeated message field on a vector of messages.
absl::StatusOr<std::optional<DataSlice>> FromProtoMessageField(
    const absl::Nonnull<DataBagPtr>& db,
    absl::string_view attr_name,
    absl::string_view field_name,
    const FieldDescriptor& field_descriptor,
    absl::Span<const absl::Nonnull<const Message*>> parent_messages,
    const std::optional<DataSlice>& parent_itemid,
    const std::optional<DataSlice>& parent_schema,
    absl::Nullable<const ExtensionMap*> parent_extension_map,
    bool ignore_field_presence = false) {
  bool is_empty = true;
  arolla::DenseArrayBuilder<arolla::Unit> mask_builder(parent_messages.size());
  std::vector<absl::Nonnull<const Message*>> packed_child_messages;
  packed_child_messages.reserve(parent_messages.size());
  for (int64_t i = 0; i < parent_messages.size(); ++i) {
    const auto* parent_message = parent_messages[i];
    const auto* refl = parent_message->GetReflection();
    if (ignore_field_presence ||
        refl->HasField(*parent_message, &field_descriptor)) {
      packed_child_messages.push_back(
          &refl->GetMessage(*parent_message, &field_descriptor));
      mask_builder.Add(i, arolla::kUnit);
      is_empty = false;
    }
  }
  if (is_empty) {
    return std::nullopt;
  }

  ASSIGN_OR_RETURN(auto schema, GetChildAttrSchema(parent_schema, attr_name));
  ASSIGN_OR_RETURN(auto itemid,
                   MakeChildObjectAttrItemIds(parent_itemid, attr_name));
  const auto* extension_map =
      GetChildExtensionMap(parent_extension_map, field_name);

  ASSIGN_OR_RETURN(
      auto mask,
      DataSlice::Create(
          internal::DataSliceImpl::Create(std::move(mask_builder).Build()),
          DataSlice::JaggedShape::FlatFromSize(parent_messages.size()),
          internal::DataItem(schema::kMask)));
  ASSIGN_OR_RETURN(
      auto packed_itemid, [&]() -> absl::StatusOr<std::optional<DataSlice>> {
        if (!itemid.has_value()) {
          return std::nullopt;
        }
        ASSIGN_OR_RETURN(auto packed_itemid, ops::Select(*itemid, mask, false));
        return packed_itemid;
      }());
  ASSIGN_OR_RETURN(
      auto packed_values,
      FromProtoMessage(db, *field_descriptor.message_type(),
                       packed_child_messages, std::move(packed_itemid), schema,
                       extension_map));

  return ops::ReverseSelect(std::move(packed_values), std::move(mask));
}

// Returns a rank-1 DataSlice of primitives converted from a proto non-repeated
// primitive field on a vector of messages.
absl::StatusOr<std::optional<DataSlice>> FromProtoPrimitiveField(
    absl::string_view attr_name, const FieldDescriptor& field_descriptor,
    absl::Span<const absl::Nonnull<const Message*>> parent_messages,
    const std::optional<DataSlice>& parent_schema,
    bool ignore_field_presence = false) {
  auto to_slice = [&]<typename T, typename F>(
                      F get) -> absl::StatusOr<std::optional<DataSlice>> {
    bool is_empty = true;
    arolla::DenseArrayBuilder<T> builder(parent_messages.size());
    for (int64_t i = 0; i < parent_messages.size(); ++i) {
      const auto& parent_message = *parent_messages[i];
      const auto* refl = parent_message.GetReflection();
      if (ignore_field_presence || !field_descriptor.has_presence() ||
          refl->HasField(parent_message, &field_descriptor)) {
        builder.Add(i, get(*refl, *parent_messages[i]));
        is_empty = false;
      }
    }
    if (is_empty) {
      return std::nullopt;
    }

    ASSIGN_OR_RETURN(
        DataSlice result,
        DataSlice::Create(
            internal::DataSliceImpl::Create(std::move(builder).Build()),
            DataSlice::JaggedShape::FlatFromSize(parent_messages.size()),
            internal::DataItem(schema::GetDType<T>())));
    ASSIGN_OR_RETURN(auto schema, GetChildAttrSchema(parent_schema, attr_name));
    if (schema.has_value() && schema->item() != schema::kObject) {
      // We could probably improve performance by using the correct backing
      // DenseArray type based on `schema` instead of casting afterward, but
      // that has a lot more cases to handle, and only has an effect if the
      // user provides an explicit schema that disagrees with the proto field
      // schemas, which should be rare.
      //
      // `validate_schema` is a no-op for primitives, so we disable it.
      return CastToExplicit(std::move(result), schema->item(),
                            /*validate_schema=*/false);
    }
    return std::move(result);
  };

  switch (field_descriptor.cpp_type()) {
    case FieldDescriptor::CPPTYPE_INT32:
      return to_slice.operator()<int32_t>(
          [&field_descriptor](const Reflection& refl, const Message& message) {
            return refl.GetInt32(message, &field_descriptor);
          });
    case FieldDescriptor::CPPTYPE_INT64:
      return to_slice.operator()<int64_t>(
          [&field_descriptor](const Reflection& refl, const Message& message) {
            return refl.GetInt64(message, &field_descriptor);
          });
    case FieldDescriptor::CPPTYPE_UINT32:
      return to_slice.operator()<int64_t>(
          [&field_descriptor](const Reflection& refl, const Message& message) {
            return refl.GetUInt32(message, &field_descriptor);
          });
    case FieldDescriptor::CPPTYPE_UINT64:
      return to_slice.operator()<int64_t>(
          [&field_descriptor](const Reflection& refl, const Message& message) {
            return refl.GetUInt64(message, &field_descriptor);
          });
    case FieldDescriptor::CPPTYPE_DOUBLE:
      return to_slice.operator()<double>(
          [&field_descriptor](const Reflection& refl, const Message& message) {
            return refl.GetDouble(message, &field_descriptor);
          });
    case FieldDescriptor::CPPTYPE_FLOAT:
      return to_slice.operator()<float>(
          [&field_descriptor](const Reflection& refl, const Message& message) {
            return refl.GetFloat(message, &field_descriptor);
          });
    case FieldDescriptor::CPPTYPE_BOOL:
      return to_slice.operator()<bool>(
          [&field_descriptor](const Reflection& refl, const Message& message) {
            return refl.GetBool(message, &field_descriptor);
          });
    case FieldDescriptor::CPPTYPE_ENUM:
      return to_slice.operator()<int32_t>(
          [&field_descriptor](const Reflection& refl, const Message& message) {
            return refl.GetEnumValue(message, &field_descriptor);
          });
    case FieldDescriptor::CPPTYPE_STRING:
      if (field_descriptor.type() == FieldDescriptor::TYPE_STRING) {
        return to_slice.operator()<arolla::Text>(
            [&field_descriptor](const Reflection& refl,
                                const Message& message) {
              return refl.GetString(message, &field_descriptor);
            });
      } else {  // TYPE_BYTES
        return to_slice.operator()<arolla::Bytes>(
            [&field_descriptor](const Reflection& refl,
                                const Message& message) {
              return refl.GetString(message, &field_descriptor);
            });
      }
    default:
      return absl::InvalidArgumentError(absl::StrFormat(
          "unexpected proto field C++ type %d", field_descriptor.cpp_type()));
  }
}

// Returns a rank-1 DataSlice converted from a proto field (of any kind) on a
// vector of messages.
absl::StatusOr<std::optional<DataSlice>> FromProtoField(
    const absl::Nonnull<DataBagPtr>& db, absl::string_view attr_name,
    absl::string_view field_name, const FieldDescriptor& field_descriptor,
    absl::Span<const absl::Nonnull<const Message*>> parent_messages,
    const std::optional<DataSlice>& parent_itemid,
    const std::optional<DataSlice>& parent_schema,
    absl::Nullable<const ExtensionMap*> parent_extension_map,
    bool ignore_field_presence) {
  if (field_descriptor.is_map()) {
    return DictFromProtoMapField(db, attr_name, field_name, field_descriptor,
                                 parent_messages, parent_itemid, parent_schema,
                                 parent_extension_map);
  } else if (field_descriptor.is_repeated()) {
    if (field_descriptor.message_type() != nullptr) {
      return ListFromProtoRepeatedMessageField(
          db, attr_name, field_name, field_descriptor, parent_messages,
          parent_itemid, parent_schema, parent_extension_map);
    } else {
      return ListFromProtoRepeatedPrimitiveField(
          db, attr_name, field_name, field_descriptor, parent_messages,
          parent_itemid, parent_schema);
    }
  } else {
    if (field_descriptor.message_type() != nullptr) {
      return FromProtoMessageField(
          db, attr_name, field_name, field_descriptor, parent_messages,
          parent_itemid, parent_schema, parent_extension_map,
          /*ignore_field_presence=*/ignore_field_presence);
    } else {
      return FromProtoPrimitiveField(
          attr_name, field_descriptor, parent_messages, parent_schema,
          /*ignore_field_presence=*/ignore_field_presence);
    }
  }
}

// Returns a size-0 rank-1 DataSlice "converted" from a vector of 0 proto
// messages.
absl::StatusOr<DataSlice> FromZeroProtoMessages(
    const absl::Nonnull<DataBagPtr>& db,
    const std::optional<DataSlice>& schema) {
  if (schema.has_value()) {
    return DataSlice::Create(
        internal::DataSliceImpl::CreateEmptyAndUnknownType(0),
        DataSlice::JaggedShape::FlatFromSize(0), schema->item(), db);
  }
  return DataSlice::Create(
      internal::DataSliceImpl::CreateEmptyAndUnknownType(0),
      DataSlice::JaggedShape::FlatFromSize(0),
      internal::DataItem(schema::kObject), db);
}

// Returns a rank-1 DataSlice of objects or entities converted from a vector of
// uniform-type proto messages.
absl::StatusOr<DataSlice> FromProtoMessage(
    const absl::Nonnull<DataBagPtr>& db, const Descriptor& message_descriptor,
    absl::Span<const absl::Nonnull<const Message*>> messages,
    const std::optional<DataSlice>& itemid,
    const std::optional<DataSlice>& schema,
    absl::Nullable<const ExtensionMap*> extension_map) {
  DCHECK(!messages.empty());

  // Defined here to maintain lifetime for references in `attr_names`.
  DataSlice::AttrNamesSet schema_attr_names;

  std::vector<
      std::tuple<absl::Nonnull<const FieldDescriptor*>, absl::string_view>>
      fields_and_attr_names;
  if (schema.has_value() && schema->IsEntitySchema()) {
    // For explicit entity schemas, use the schema attr names as the list of
    // fields and extensions to convert.
    ASSIGN_OR_RETURN(schema_attr_names, schema->GetAttrNames());
    fields_and_attr_names.reserve(schema_attr_names.size());
    for (const auto& attr_name : schema_attr_names) {
      if (attr_name.starts_with('(') && attr_name.ends_with(')')) {
        // Interpret attrs with parentheses as fully-qualified extension paths.
        const auto ext_full_path =
            absl::string_view(attr_name).substr(1, attr_name.size() - 2);
        const auto* field =
            message_descriptor.file()->pool()->FindExtensionByName(
                ext_full_path);
        if (field == nullptr) {
          return absl::InvalidArgumentError(
              absl::StrFormat("extension not found: \"%s\"", ext_full_path));
        }
        fields_and_attr_names.emplace_back(field, attr_name);
      } else {
        const auto* field = message_descriptor.FindFieldByName(attr_name);
        if (field != nullptr) {
          fields_and_attr_names.emplace_back(field, attr_name);
        }
      }
    }
  } else {
    // For unset and OBJECT schemas, convert all fields + requested extensions.
    const int64_t num_fields =
        message_descriptor.field_count() +
        ((extension_map != nullptr) ? extension_map->extension_fields.size()
                                    : 0);
    fields_and_attr_names.reserve(num_fields);
    for (int i_field = 0; i_field < message_descriptor.field_count();
         ++i_field) {
      const auto* field = message_descriptor.field(i_field);
      fields_and_attr_names.emplace_back(field, field->name());
    }
    if (extension_map != nullptr) {
      for (const auto& [attr_name, field] : extension_map->extension_fields) {
        fields_and_attr_names.emplace_back(field, attr_name);
      }
    }
  }

  std::vector<absl::string_view> value_attr_names;
  std::vector<DataSlice> values;
  for (const auto& [field, attr_name] : fields_and_attr_names) {
    ASSIGN_OR_RETURN(std::optional<DataSlice> field_values,
                     FromProtoField(db, attr_name, attr_name, *field,
                                    messages, itemid, schema, extension_map));
    if (field_values.has_value()) {
      DCHECK(!field_values->IsEmpty());
      values.push_back(std::move(field_values).value());
      value_attr_names.push_back(attr_name);
    }
  }

  auto result_shape = DataSlice::JaggedShape::FlatFromSize(messages.size());
  if (schema.has_value()) {
    RETURN_IF_ERROR(schema->VerifyIsSchema());
    if (schema->item() == schema::kObject) {
      return ObjectCreator::Shaped(db, std::move(result_shape),
                                   /*attr_names=*/std::move(value_attr_names),
                                   /*values=*/std::move(values),
                                   /*itemid=*/itemid);
    } else {  // schema != OBJECT
      return EntityCreator::Shaped(db, std::move(result_shape),
                                   /*attr_names=*/std::move(value_attr_names),
                                   /*values=*/std::move(values),
                                   /*schema=*/schema,
                                   /*update_schema=*/false,
                                   /*itemid=*/itemid);
    }
  } else {  // schema == nullopt
    ASSIGN_OR_RETURN(DataSlice bare_schema,
                     CreateBareProtoUuSchema(db, message_descriptor));
    return EntityCreator::Shaped(db, std::move(result_shape),
                                 /*attr_names=*/std::move(value_attr_names),
                                 /*values=*/std::move(values),
                                 /*schema=*/std::move(bare_schema),
                                 /*update_schema=*/true,
                                 /*itemid=*/itemid);
  }
}

}  // namespace

absl::StatusOr<DataSlice> FromProto(
    const absl::Nonnull<DataBagPtr>& db,
    absl::Span<const absl::Nonnull<const Message*>> messages,
    absl::Span<const absl::string_view> extensions,
    const std::optional<DataSlice>& itemid,
    const std::optional<DataSlice>& schema) {
  if (schema.has_value()) {
    RETURN_IF_ERROR(schema->VerifyIsSchema());
    AdoptionQueue adoption_queue;
    adoption_queue.Add(*schema);
    RETURN_IF_ERROR(adoption_queue.AdoptInto(*db));
  }

  if (messages.empty()) {
    return FromZeroProtoMessages(db, schema);
  }

  const Descriptor* message_descriptor = messages[0]->GetDescriptor();
  for (const Message* message : messages) {
    if (message->GetDescriptor() != message_descriptor) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "expected all messages to have the same type, got %s and %s",
          message_descriptor->full_name(),
          message->GetDescriptor()->full_name()));
    }
  }

  ASSIGN_OR_RETURN(
      const ExtensionMap extension_map,
      ParseExtensions(extensions, *message_descriptor->file()->pool()));

  return FromProtoMessage(db, *message_descriptor, messages, itemid, schema,
                          &extension_map);
}

}  // namespace koladata
