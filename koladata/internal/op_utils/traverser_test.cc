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
#include "koladata/internal/op_utils/traverser.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "koladata/internal/data_bag.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/data_slice.h"
#include "koladata/internal/dtype.h"
#include "koladata/internal/object_id.h"
#include "koladata/internal/schema_utils.h"
#include "arolla/dense_array/dense_array.h"
#include "arolla/dense_array/edge.h"
#include "arolla/memory/optional_value.h"
#include "arolla/util/text.h"
#include "arolla/util/status_macros_backport.h"

namespace koladata::internal {
namespace {

using ::arolla::CreateDenseArray;

using TriplesT = std::vector<
    std::pair<DataItem, std::vector<std::pair<std::string_view, DataItem>>>>;

DataItem AllocateSchema() {
  return DataItem(internal::AllocateExplicitSchema());
}

template <typename T>
DataSliceImpl CreateSlice(absl::Span<const arolla::OptionalValue<T>> values) {
  return DataSliceImpl::Create(CreateDenseArray<T>(values));
}

void SetSchemaTriples(DataBagImpl& db, const TriplesT& schema_triples) {
  for (auto [schema, attrs] : schema_triples) {
    for (auto [attr_name, attr_schema] : attrs) {
      EXPECT_OK(db.SetSchemaAttr(schema, attr_name, attr_schema));
    }
  }
}

void SetDataTriples(DataBagImpl& db, const TriplesT& data_triples) {
  for (auto [item, attrs] : data_triples) {
    for (auto [attr_name, attr_data] : attrs) {
      EXPECT_OK(db.SetAttr(item, attr_name, attr_data));
    }
  }
}

TriplesT GenNoiseDataTriples() {
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(5);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto a3 = obj_ids[3];
  auto a4 = obj_ids[4];
  TriplesT data = {{a0, {{"x", DataItem(1)}, {"next", a1}}},
                   {a1, {{"y", DataItem(3)}, {"prev", a0}, {"next", a2}}},
                   {a3, {{"x", DataItem(1)}, {"y", DataItem(2)}, {"next", a4}}},
                   {a4, {{"prev", a3}}}};
  return data;
}

TriplesT GenNoiseSchemaTriples() {
  auto schema0 = AllocateSchema();
  auto schema1 = AllocateSchema();
  auto int_dtype = DataItem(schema::kInt32);
  TriplesT schema_triples = {
      {schema0, {{"self", schema0}, {"next", schema1}, {"x", int_dtype}}},
      {schema1, {{"prev", schema0}, {"y", int_dtype}}}};
  return schema_triples;
}

enum TraverserTestParam { kMainDb, kFallbackDb };

class TraversingOpTest : public ::testing::TestWithParam<TraverserTestParam> {
 public:
  DataBagImplPtr GetMainDb(DataBagImplPtr db) {
    switch (GetParam()) {
      case kMainDb:
        return db;
      case kFallbackDb:
        return DataBagImpl::CreateEmptyDatabag();
    }
    DCHECK(false);
  }
  DataBagImplPtr GetFallbackDb(DataBagImplPtr db) {
    switch (GetParam()) {
      case kMainDb:
        return DataBagImpl::CreateEmptyDatabag();
      case kFallbackDb:
        return db;
    }
    DCHECK(false);
  }
};

class NoOpTraverserTest : public TraversingOpTest {};

INSTANTIATE_TEST_SUITE_P(MainOrFallback, NoOpTraverserTest,
                         ::testing::Values(kMainDb, kFallbackDb));

class ObjectsTraverserTest : public TraversingOpTest {};

INSTANTIATE_TEST_SUITE_P(MainOrFallback, ObjectsTraverserTest,
                         ::testing::Values(kMainDb, kFallbackDb));



class NoOpVisitor : AbstractVisitor {
 public:
  explicit NoOpVisitor()
      : previsited_(), value_item_(DataItem("get value result")) {}

  absl::Status Previsit(const DataItem& item, const DataItem& schema) override {
    previsited_.push_back({item, schema});
    return absl::OkStatus();
  }

  absl::StatusOr<DataItem> GetValue(const DataItem& item,
                                    const DataItem& schema) override {
    if (std::find(previsited_.begin(), previsited_.end(),
                  std::make_pair(item, schema)) == previsited_.end()) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "item %v with schema %v is not previsited", item, schema));
    }
    return value_item_;
  }

  absl::Status VisitList(const DataItem& list, const DataItem& schema,
                         bool is_object_schema,
                         const DataSliceImpl& items) override {
    DCHECK(list.holds_value<ObjectId>() && list.value<ObjectId>().IsList());
    RETURN_IF_ERROR(CheckValues(items));
    return absl::OkStatus();
  }

  absl::Status VisitDict(const DataItem& dict, const DataItem& schema,
                         bool is_object_schema, const DataSliceImpl& keys,
                         const DataSliceImpl& values) override {
    DCHECK(dict.holds_value<ObjectId>() && dict.value<ObjectId>().IsDict());
    RETURN_IF_ERROR(CheckValues(keys));
    RETURN_IF_ERROR(CheckValues(values));
    return absl::OkStatus();
  }

  absl::Status VisitObject(
      const DataItem& object, const DataItem& schema, bool is_object_schema,
      const arolla::DenseArray<arolla::Text>& attr_names,
      const arolla::DenseArray<DataItem>& attr_values) override {
    DCHECK(object.holds_value<ObjectId>());
    RETURN_IF_ERROR(CheckValues(DataSliceImpl::Create(attr_values)));
    return absl::OkStatus();
  }

  absl::Status VisitSchema(
      const DataItem& item, const DataItem& schema, bool is_object_schema,
      const arolla::DenseArray<arolla::Text>& attr_names,
      const arolla::DenseArray<DataItem>& attr_schema) override {
    return VisitObject(item, schema, is_object_schema, attr_names,
                       attr_schema);
  }

  absl::Status VisitPrimitive(const DataItem& item,
                              const DataItem& schema) override {
    return absl::OkStatus();
  }

 private:
  absl::Status CheckValues(const DataSliceImpl& items) {
    for (int i = 0; i < items.size(); ++i) {
      if (items[i] != value_item_) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "expected result of GetValue call, got %v", items[i]));
      }
    }
    return absl::OkStatus();
  }

  std::vector<std::pair<DataItem, DataItem>> previsited_;
  DataItem value_item_;
};

absl::Status TraverseSlice(const DataSliceImpl& ds, const DataItem& schema,
                           const DataBagImpl& databag,
                           DataBagImpl::FallbackSpan fallbacks) {
  auto visitor = std::make_shared<NoOpVisitor>();
  auto traverse_op = Traverser<NoOpVisitor>(databag, fallbacks, visitor);
  RETURN_IF_ERROR(traverse_op.TraverseSlice(ds, schema));
  return absl::OkStatus();
}

class ObjectVisitor : AbstractVisitor {
 public:
  explicit ObjectVisitor() = default;

  absl::Status Previsit(const DataItem& item, const DataItem& schema) override {
    if (!item.holds_value<ObjectId>()) {
      return absl::OkStatus();
    }
    if (schema == schema::kObject) {
      previsited_objects_.insert(item.value<ObjectId>());
    } else if (schema.holds_value<ObjectId>()){
      if (!previsited_objects_.contains(item.value<ObjectId>())) {
        return absl::InternalError(
            absl::StrFormat("object %v is previsited with schema %v first",
                            item, schema));
      }
      previsited_with_schema_.insert(item.value<ObjectId>());
    }
    return absl::OkStatus();
  }

  absl::StatusOr<DataItem> GetValue(const DataItem& item,
                                    const DataItem& schema) override {
    return item;
  }

  absl::Status VisitList(const DataItem& list, const DataItem& schema,
                         bool is_object_schema,
                         const DataSliceImpl& items) override {
    return absl::OkStatus();
  }

  absl::Status VisitDict(const DataItem& dict, const DataItem& schema,
                         bool is_object_schema, const DataSliceImpl& keys,
                         const DataSliceImpl& values) override {
    return absl::OkStatus();
  }

  absl::Status VisitObject(
      const DataItem& object, const DataItem& schema, bool is_object_schema,
      const arolla::DenseArray<arolla::Text>& attr_names,
      const arolla::DenseArray<DataItem>& attr_values) override {
    return absl::OkStatus();
  }

  absl::Status VisitSchema(
      const DataItem& item, const DataItem& schema, bool is_object_schema,
      const arolla::DenseArray<arolla::Text>& attr_names,
      const arolla::DenseArray<DataItem>& attr_schema) override {
    return absl::OkStatus();
  }

  absl::Status VisitPrimitive(const DataItem& item,
                              const DataItem& schema) override {
    return absl::OkStatus();
  }

  absl::Status check_previsited_objects_twice() const {
    if (previsited_objects_.size() != previsited_with_schema_.size()) {
      return absl::InternalError("not all objects are previsited twice");
    }
    return absl::OkStatus();
  }

 private:
  absl::flat_hash_set<ObjectId> previsited_objects_;
  absl::flat_hash_set<ObjectId> previsited_with_schema_;
};

absl::Status TraverseSliceCheckObjectPrevisits(
    const DataSliceImpl& ds, const DataItem& schema, const DataBagImpl& databag,
    DataBagImpl::FallbackSpan fallbacks) {
  auto visitor = std::make_shared<ObjectVisitor>();
  auto traverse_op = Traverser<ObjectVisitor>(databag, fallbacks, visitor);
  RETURN_IF_ERROR(traverse_op.TraverseSlice(ds, schema));
  return visitor->check_previsited_objects_twice();
}

TEST_P(NoOpTraverserTest, ShallowEntitySlice) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(3);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto int_dtype = DataItem(schema::kInt32);
  auto schema = AllocateSchema();

  TriplesT schema_triples = {{schema, {{"x", int_dtype}, {"y", int_dtype}}}};
  TriplesT data_triples = {{a0, {{"x", DataItem(1)}, {"y", DataItem(4)}}},
                           {a1, {{"x", DataItem(2)}, {"y", DataItem(5)}}},
                           {a2, {{"x", DataItem(3)}, {"y", DataItem(6)}}}};
  SetSchemaTriples(*db, schema_triples);
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto result_db = DataBagImpl::CreateEmptyDatabag();
  EXPECT_OK(TraverseSlice(obj_ids, schema, *GetMainDb(db),
                          {GetFallbackDb(db).get()}));
}

TEST_P(NoOpTraverserTest, DeepEntitySlice) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(6);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto b0 = obj_ids[3];
  auto b1 = obj_ids[4];
  auto b2 = obj_ids[5];
  auto ds =
      DataSliceImpl::Create(arolla::CreateDenseArray<DataItem>({a0, a1, a2}));
  auto schema_a = AllocateSchema();
  auto schema_b = AllocateSchema();
  TriplesT data_triples = {{a0, {{"self", a0}, {"b", b0}}},
                           {a1, {{"self", DataItem()}, {"b", b1}}},
                           {a2, {{"self", a2}, {"b", b2}}},
                           {b0, {{"self", b0}}},
                           {b1, {{"self", b1}}},
                           {b2, {{"self", b2}}}};
  TriplesT schema_triples = {{schema_a, {{"self", schema_a}, {"b", schema_b}}},
                             {schema_b, {{"self", schema_b}}}};
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto result_db = DataBagImpl::CreateEmptyDatabag();
  EXPECT_OK(
      TraverseSlice(ds, schema_a, *GetMainDb(db), {GetFallbackDb(db).get()}));
}

TEST_P(NoOpTraverserTest, ShallowListsSlice) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto lists = DataSliceImpl::ObjectsFromAllocation(AllocateLists(3), 3);
  auto values =
      DataSliceImpl::Create(CreateDenseArray<int32_t>({1, 2, 3, 4, 5, 6, 7}));
  ASSERT_OK_AND_ASSIGN(auto edge, arolla::DenseArrayEdge::FromSplitPoints(
                                      CreateDenseArray<int64_t>({0, 3, 5, 7})));
  ASSERT_OK(db->ExtendLists(lists, values, edge));
  auto list_schema = AllocateSchema();
  TriplesT schema_triples = {
      {list_schema,
       {{schema::kListItemsSchemaAttr, DataItem(schema::kInt32)}}}};
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto result_db = DataBagImpl::CreateEmptyDatabag();
  EXPECT_OK(TraverseSlice(lists, list_schema, *GetMainDb(db),
                          {GetFallbackDb(db).get()}));
}

TEST_P(NoOpTraverserTest, DeepListsSlice) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto lists = DataSliceImpl::ObjectsFromAllocation(AllocateLists(3), 3);
  auto sparse_lists = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {lists[0], DataItem(), DataItem(), lists[1], lists[2]}));
  auto values = DataSliceImpl::AllocateEmptyObjects(7);
  auto sparse_values = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {values[0], DataItem(), DataItem(), values[1], values[2], values[3],
       values[4], values[5], values[6], DataItem()}));
  ASSERT_OK_AND_ASSIGN(auto edge,
                       arolla::DenseArrayEdge::FromSplitPoints(
                           CreateDenseArray<int64_t>({0, 5, 7, 10})));
  ASSERT_OK(db->ExtendLists(lists, sparse_values, edge));
  auto item_schema = AllocateSchema();
  auto list_schema = AllocateSchema();
  TriplesT data_triples = {
      {values[0], {{"x", DataItem(1)}}}, {values[1], {{"x", DataItem(2)}}},
      {values[2], {{"x", DataItem(3)}}}, {values[3], {{"x", DataItem(4)}}},
      {values[4], {{"x", DataItem(5)}}}, {values[5], {{"x", DataItem(6)}}},
      {values[6], {{"x", DataItem(7)}}}};
  TriplesT schema_triples = {
      {list_schema, {{schema::kListItemsSchemaAttr, item_schema}}},
      {item_schema, {{"x", DataItem(schema::kInt32)}}}};
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto result_db = DataBagImpl::CreateEmptyDatabag();
  EXPECT_OK(TraverseSlice(sparse_lists, list_schema, *GetMainDb(db),
                          {GetFallbackDb(db).get()}));
}

TEST_P(NoOpTraverserTest, ShallowDictsSlice) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto dicts = DataSliceImpl::ObjectsFromAllocation(AllocateDicts(3), 3);
  auto dicts_expanded = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {dicts[0], dicts[0], dicts[0], dicts[1], dicts[1], dicts[2], dicts[2]}));
  auto keys =
      DataSliceImpl::Create(CreateDenseArray<int64_t>({1, 2, 3, 1, 5, 3, 7}));
  auto values =
      DataSliceImpl::Create(CreateDenseArray<float>({1, 2, 3, 4, 5, 6, 7}));
  ASSERT_OK(db->SetInDict(dicts_expanded, keys, values));
  auto dict_schema = AllocateSchema();
  TriplesT schema_triples = {
      {dict_schema,
       {{schema::kDictKeysSchemaAttr, DataItem(schema::kInt32)},
        {schema::kDictValuesSchemaAttr, DataItem(schema::kFloat32)}}}};
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto result_db = DataBagImpl::CreateEmptyDatabag();
  EXPECT_OK(TraverseSlice(dicts, dict_schema, *GetMainDb(db),
                          {GetFallbackDb(db).get()}));
}

TEST_P(NoOpTraverserTest, DeepDictsSlice) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto dicts = DataSliceImpl::ObjectsFromAllocation(AllocateDicts(3), 3);
  auto sparse_dicts = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {dicts[0], DataItem(), DataItem(), dicts[1], dicts[2]}));
  auto keys = DataSliceImpl::AllocateEmptyObjects(4);
  auto values = DataSliceImpl::AllocateEmptyObjects(7);
  auto dicts_expanded = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {dicts[0], dicts[0], dicts[0], dicts[1], dicts[1], dicts[2], dicts[2]}));
  auto keys_expanded = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {keys[0], keys[1], keys[2], keys[0], keys[3], keys[2], keys[3]}));
  ASSERT_OK(db->SetInDict(dicts_expanded, keys_expanded, values));
  auto key_schema = AllocateSchema();
  auto value_schema = AllocateSchema();
  auto dict_schema = AllocateSchema();
  TriplesT data_triples = {{keys[0], {{"name", DataItem("a")}}},
                           {keys[1], {{"name", DataItem("b")}}},
                           {keys[2], {{"name", DataItem("c")}}},
                           {keys[3], {{"name", DataItem("d")}}},
                           {values[0], {{"x", DataItem(1)}}},
                           {values[1], {{"x", DataItem(2)}}},
                           {values[2], {{"x", DataItem(3)}}},
                           {values[3], {{"x", DataItem(4)}}},
                           {values[4], {{"x", DataItem(5)}}},
                           {values[5], {{"x", DataItem(6)}}},
                           {values[6], {{"x", DataItem(7)}}}};
  TriplesT schema_triples = {
      {key_schema, {{"name", DataItem(schema::kString)}}},
      {value_schema, {{"x", DataItem(schema::kInt32)}}},
      {dict_schema,
       {{schema::kDictKeysSchemaAttr, key_schema},
        {schema::kDictValuesSchemaAttr, value_schema}}}};
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto result_db = DataBagImpl::CreateEmptyDatabag();
  EXPECT_OK(TraverseSlice(sparse_dicts, dict_schema, *GetMainDb(db),
                          {GetFallbackDb(db).get()}));
}

TEST_P(NoOpTraverserTest, ObjectsSlice) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(10);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto a3 = obj_ids[3];
  auto a4 = obj_ids[4];
  auto a5 = obj_ids[5];
  auto dicts = DataSliceImpl::ObjectsFromAllocation(AllocateDicts(2), 2);
  auto lists = DataSliceImpl::ObjectsFromAllocation(AllocateLists(2), 2);
  ASSERT_OK(db->SetInDict(dicts[0], DataItem("a"), DataItem(1)));
  ASSERT_OK(db->SetInDict(dicts[1], a2, a3));
  ASSERT_OK(db->ExtendList(
      lists[0], DataSliceImpl::Create(CreateDenseArray<DataItem>({a4, a5}))));
  ASSERT_OK(db->ExtendList(
      lists[1], DataSliceImpl::Create(CreateDenseArray<int32_t>({0, 1, 2}))));
  auto item_schema = AllocateSchema();
  auto key_schema = AllocateSchema();
  auto dict0_schema = AllocateSchema();
  auto dict1_schema = AllocateSchema();
  auto list0_schema = AllocateSchema();
  auto list1_schema = AllocateSchema();
  TriplesT data_triples = {
      {a0, {{schema::kSchemaAttr, item_schema}, {"x", DataItem(1)}}},
      {a1, {{schema::kSchemaAttr, item_schema}, {"x", DataItem(2)}}},
      {a2, {{"name", DataItem("k0")}}},
      {a3, {{"x", DataItem(10)}}},
      {a4, {{schema::kSchemaAttr, item_schema}, {"x", DataItem(3)}}},
      {a5, {{schema::kSchemaAttr, item_schema}, {"x", DataItem(4)}}},
      {dicts[0], {{schema::kSchemaAttr, dict0_schema}}},
      {dicts[1], {{schema::kSchemaAttr, dict1_schema}}},
      {lists[0], {{schema::kSchemaAttr, list0_schema}}},
      {lists[1], {{schema::kSchemaAttr, list1_schema}}},
  };
  TriplesT schema_triples = {
      {item_schema, {{"x", DataItem(schema::kInt32)}}},
      {key_schema, {{"name", DataItem(schema::kString)}}},
      {dict0_schema,
       {{schema::kDictKeysSchemaAttr, DataItem(schema::kString)},
        {schema::kDictValuesSchemaAttr, DataItem(schema::kInt32)}}},
      {dict1_schema,
       {{schema::kDictKeysSchemaAttr, key_schema},
        {schema::kDictValuesSchemaAttr, item_schema}}},
      {list0_schema, {{schema::kListItemsSchemaAttr, item_schema}}},
      {list1_schema,
       {{schema::kListItemsSchemaAttr, DataItem(schema::kInt32)}}}};
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, schema_triples);
  SetDataTriples(*db, GenNoiseDataTriples());
  SetSchemaTriples(*db, GenNoiseSchemaTriples());

  auto ds = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {a0, a1, DataItem(), DataItem(3), DataItem("a"), dicts[0], dicts[1],
       lists[0], lists[1]}));
  auto schema = AllocateSchema();
  auto result_db = DataBagImpl::CreateEmptyDatabag();
  EXPECT_OK(TraverseSlice(ds, DataItem(schema::kObject), *GetMainDb(db),
                          {GetFallbackDb(db).get()}));
}

TEST_P(NoOpTraverserTest, SchemaSlice) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto s1 = AllocateSchema();
  auto s2 = AllocateSchema();
  TriplesT schema_triples = {
      {s1, {{"x", DataItem(schema::kInt32)}}},
      {s2, {{"a", DataItem(schema::kString)}}},
  };
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto ds = DataSliceImpl::Create(CreateDenseArray<DataItem>({s1, s2}));
  auto result_db = DataBagImpl::CreateEmptyDatabag();
  EXPECT_OK(TraverseSlice(ds, DataItem(schema::kSchema), *GetMainDb(db),
                          {GetFallbackDb(db).get()}));
}

TEST_P(ObjectsTraverserTest, ObjectsSlice) {
    auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(10);
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];
  auto a3 = obj_ids[3];
  auto a4 = obj_ids[4];
  auto a5 = obj_ids[5];
  auto dicts = DataSliceImpl::ObjectsFromAllocation(AllocateDicts(2), 2);
  auto lists = DataSliceImpl::ObjectsFromAllocation(AllocateLists(2), 2);
  ASSERT_OK(db->SetInDict(dicts[0], DataItem("a"), DataItem(1)));
  ASSERT_OK(db->SetInDict(dicts[1], a2, a3));
  ASSERT_OK(db->ExtendList(
      lists[0], DataSliceImpl::Create(CreateDenseArray<DataItem>({a4, a5}))));
  ASSERT_OK(db->ExtendList(
      lists[1], DataSliceImpl::Create(CreateDenseArray<int32_t>({0, 1, 2}))));
  auto item_schema = AllocateSchema();
  auto key_schema = AllocateSchema();
  auto dict0_schema = AllocateSchema();
  auto dict1_schema = AllocateSchema();
  auto list0_schema = AllocateSchema();
  auto list1_schema = AllocateSchema();
  TriplesT data_triples = {
      {a0, {{schema::kSchemaAttr, item_schema}, {"x", DataItem(1)}}},
      {a1, {{schema::kSchemaAttr, item_schema}, {"x", DataItem(2)}}},
      {a2, {{schema::kSchemaAttr, key_schema}, {"name", DataItem("k0")}}},
      {a3, {{schema::kSchemaAttr, item_schema}, {"x", DataItem(10)}}},
      {a4, {{schema::kSchemaAttr, item_schema}, {"x", DataItem(3)}}},
      {a5, {{schema::kSchemaAttr, item_schema}, {"x", DataItem(4)}}},
      {dicts[0], {{schema::kSchemaAttr, dict0_schema}}},
      {dicts[1], {{schema::kSchemaAttr, dict1_schema}}},
      {lists[0], {{schema::kSchemaAttr, list0_schema}}},
      {lists[1], {{schema::kSchemaAttr, list1_schema}}},
  };
  TriplesT schema_triples = {
      {item_schema, {{"x", DataItem(schema::kInt32)}}},
      {key_schema, {{"name", DataItem(schema::kString)}}},
      {dict0_schema,
       {{schema::kDictKeysSchemaAttr, DataItem(schema::kString)},
        {schema::kDictValuesSchemaAttr, DataItem(schema::kInt32)}}},
      {dict1_schema,
       {{schema::kDictKeysSchemaAttr, DataItem(schema::kObject)},
        {schema::kDictValuesSchemaAttr, DataItem(schema::kObject)}}},
      {list0_schema,
       {{schema::kListItemsSchemaAttr, DataItem(schema::kObject)}}},
      {list1_schema,
       {{schema::kListItemsSchemaAttr, DataItem(schema::kInt32)}}}};
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, schema_triples);
  SetDataTriples(*db, GenNoiseDataTriples());
  SetSchemaTriples(*db, GenNoiseSchemaTriples());

  auto ds = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {a0, a1, DataItem(), DataItem(3), DataItem("a"), dicts[0], dicts[1],
       lists[0], lists[1]}));
  auto schema = AllocateSchema();
  auto result_db = DataBagImpl::CreateEmptyDatabag();
  EXPECT_OK(TraverseSliceCheckObjectPrevisits(ds, DataItem(schema::kObject),
                                              *GetMainDb(db),
                                              {GetFallbackDb(db).get()}));
}

}  // namespace
}  // namespace koladata::internal
