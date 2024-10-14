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
#include "koladata/internal/op_utils/deep_clone.h"

#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/log/check.h"
#include "absl/types/span.h"
#include "koladata/internal/data_bag.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/data_slice.h"
#include "koladata/internal/dtype.h"
#include "koladata/internal/object_id.h"
#include "koladata/internal/schema_utils.h"
#include "koladata/internal/testing/matchers.h"
#include "arolla/dense_array/dense_array.h"
#include "arolla/dense_array/edge.h"
#include "arolla/memory/optional_value.h"
#include "arolla/util/bytes.h"
#include "arolla/util/fingerprint.h"

namespace koladata::internal {
namespace {

using ::arolla::CreateDenseArray;
using ::koladata::internal::testing::DataBagEqual;

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

enum DeepCloneTestParam { kMainDb, kFallbackDb };

class CopyingOpTest : public ::testing::TestWithParam<DeepCloneTestParam> {
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

class DeepCloneTest : public CopyingOpTest {};

INSTANTIATE_TEST_SUITE_P(MainOrFallback, DeepCloneTest,
                         ::testing::Values(kMainDb, kFallbackDb));

TEST_P(DeepCloneTest, ShallowEntitySlice) {
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
  ASSERT_OK_AND_ASSIGN(
      (auto [result_slice, result_schema]),
      DeepCloneOp(result_db.get())(obj_ids, schema, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));
  EXPECT_EQ(result_slice.size(), 3);
  EXPECT_NE(result_slice[0], a0);
  EXPECT_NE(result_slice[1], a1);
  EXPECT_NE(result_slice[2], a2);
  TriplesT expected_data_triples = {
      {result_slice[0], {{"x", DataItem(1)}, {"y", DataItem(4)}}},
      {result_slice[1], {{"x", DataItem(2)}, {"y", DataItem(5)}}},
      {result_slice[2], {{"x", DataItem(3)}, {"y", DataItem(6)}}}};
  TriplesT expected_schema_triples = {
      {result_schema, {{"x", int_dtype}, {"y", int_dtype}}}};
  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  SetDataTriples(*expected_db, expected_data_triples);
  SetSchemaTriples(*expected_db, expected_schema_triples);
  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(expected_db));
}

TEST_P(DeepCloneTest, DeepEntitySlice) {
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
                           {a1, {{"self", a1}, {"b", b1}}},
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
  ASSERT_OK_AND_ASSIGN(
      (auto [result_slice, result_schema]),
      DeepCloneOp(result_db.get())(ds, schema_a, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));

  EXPECT_EQ(result_slice.size(), 3);
  EXPECT_EQ(result_schema, schema_a);
  EXPECT_NE(result_slice[0], a0);
  EXPECT_NE(result_slice[1], a1);
  EXPECT_NE(result_slice[2], a2);
  ASSERT_OK_AND_ASSIGN(auto result_b, result_db->GetAttr(result_slice, "b"));
  ASSERT_OK_AND_ASSIGN(auto result_b_schema,
                       result_db->GetSchemaAttr(result_schema, "b"));
  EXPECT_EQ(result_b_schema, schema_b);
  EXPECT_NE(result_b[0], b0);
  EXPECT_NE(result_b[1], b1);
  EXPECT_NE(result_b[2], b2);
  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  TriplesT expected_data_triples = {
      {result_slice[0], {{"self", result_slice[0]}, {"b", result_b[0]}}},
      {result_slice[1], {{"self", result_slice[1]}, {"b", result_b[1]}}},
      {result_slice[2], {{"self", result_slice[2]}, {"b", result_b[2]}}},
      {result_b[0], {{"self", result_b[0]}}},
      {result_b[1], {{"self", result_b[1]}}},
      {result_b[2], {{"self", result_b[2]}}},
  };
  TriplesT expected_schema_triples = {
      {result_schema, {{"self", result_schema}, {"b", result_b_schema}}},
      {result_b_schema, {{"self", result_b_schema}}},
  };
  SetDataTriples(*expected_db, expected_data_triples);
  SetSchemaTriples(*expected_db, expected_schema_triples);
  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(expected_db));
}

TEST_P(DeepCloneTest, ShallowListsSlice) {
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
  ASSERT_OK_AND_ASSIGN(
      (auto [result_slice, result_schema]),
      DeepCloneOp(result_db.get())(lists, list_schema, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));
  EXPECT_EQ(result_slice.size(), 3);
  EXPECT_EQ(result_schema, list_schema);
  EXPECT_NE(result_slice[0], lists[0]);
  EXPECT_NE(result_slice[1], lists[1]);
  EXPECT_NE(result_slice[2], lists[2]);
  EXPECT_TRUE(result_slice[0].is_list());
  EXPECT_TRUE(result_slice[1].is_list());
  EXPECT_TRUE(result_slice[2].is_list());
  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  TriplesT expected_schema_triples = {
      {result_schema,
       {{schema::kListItemsSchemaAttr, DataItem(schema::kInt32)}}}};
  ASSERT_OK(expected_db->ExtendLists(result_slice, values, edge));
  SetSchemaTriples(*expected_db, expected_schema_triples);
  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(DeepCloneTest, DeepListsSlice) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto lists = DataSliceImpl::ObjectsFromAllocation(AllocateLists(3), 3);
  auto values = DataSliceImpl::AllocateEmptyObjects(7);
  ASSERT_OK_AND_ASSIGN(auto edge, arolla::DenseArrayEdge::FromSplitPoints(
                                      CreateDenseArray<int64_t>({0, 3, 5, 7})));
  ASSERT_OK(db->ExtendLists(lists, values, edge));
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
  ASSERT_OK_AND_ASSIGN(
      (auto [result_slice, result_schema]),
      DeepCloneOp(result_db.get())(lists, list_schema, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));
  EXPECT_EQ(result_slice.size(), 3);
  EXPECT_EQ(result_schema, list_schema);
  EXPECT_NE(result_slice[0], lists[0]);
  EXPECT_NE(result_slice[1], lists[1]);
  EXPECT_NE(result_slice[2], lists[2]);
  EXPECT_TRUE(result_slice[0].is_list());
  EXPECT_TRUE(result_slice[1].is_list());
  EXPECT_TRUE(result_slice[2].is_list());
  ASSERT_OK_AND_ASSIGN(
      auto result_item_schema,
      result_db->GetSchemaAttr(result_schema, schema::kListItemsSchemaAttr));
  EXPECT_EQ(result_item_schema, item_schema);
  ASSERT_OK_AND_ASSIGN((auto [result_values, _]),
                       result_db->ExplodeLists(result_slice));
  EXPECT_EQ(result_values.present_count(), values.present_count());
  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  TriplesT expected_data_triples = {{result_values[0], {{"x", DataItem(1)}}},
                                    {result_values[1], {{"x", DataItem(2)}}},
                                    {result_values[2], {{"x", DataItem(3)}}},
                                    {result_values[3], {{"x", DataItem(4)}}},
                                    {result_values[4], {{"x", DataItem(5)}}},
                                    {result_values[5], {{"x", DataItem(6)}}},
                                    {result_values[6], {{"x", DataItem(7)}}}};
  TriplesT expected_schema_triples = {
      {result_schema, {{schema::kListItemsSchemaAttr, result_item_schema}}},
      {result_item_schema, {{"x", DataItem(schema::kInt32)}}}};
  ASSERT_OK(expected_db->ExtendLists(result_slice, result_values, edge));
  SetDataTriples(*expected_db, expected_data_triples);
  SetSchemaTriples(*expected_db, expected_schema_triples);
  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(DeepCloneTest, ShallowDictsSlice) {
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
  ASSERT_OK_AND_ASSIGN(
      (auto [result_slice, result_schema]),
      DeepCloneOp(result_db.get())(dicts, dict_schema, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));
  EXPECT_EQ(result_slice.size(), 3);
  EXPECT_EQ(result_schema, dict_schema);
  EXPECT_NE(result_slice[0], dicts[0]);
  EXPECT_NE(result_slice[1], dicts[1]);
  EXPECT_NE(result_slice[2], dicts[2]);
  EXPECT_TRUE(result_slice[0].is_dict());
  EXPECT_TRUE(result_slice[1].is_dict());
  EXPECT_TRUE(result_slice[2].is_dict());
  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  auto result_dicts_expanded = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {result_slice[0], result_slice[0], result_slice[0], result_slice[1],
       result_slice[1], result_slice[2], result_slice[2]}));
  ASSERT_OK(expected_db->SetInDict(result_dicts_expanded, keys, values));
  TriplesT expected_schema_triples = {
      {result_schema,
       {{schema::kDictKeysSchemaAttr, DataItem(schema::kInt32)},
        {schema::kDictValuesSchemaAttr, DataItem(schema::kFloat32)}}}};
  SetSchemaTriples(*expected_db, expected_schema_triples);
  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(DeepCloneTest, DeepDictsSlice) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto dicts = DataSliceImpl::ObjectsFromAllocation(AllocateDicts(3), 3);
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
  TriplesT schema_triples = {{key_schema, {{"name", DataItem(schema::kText)}}},
                             {value_schema, {{"x", DataItem(schema::kInt32)}}},
                             {dict_schema,
                              {{schema::kDictKeysSchemaAttr, key_schema},
                               {schema::kDictValuesSchemaAttr, value_schema}}}};
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto result_db = DataBagImpl::CreateEmptyDatabag();
  ASSERT_OK_AND_ASSIGN(
      (auto [result_slice, result_schema]),
      DeepCloneOp(result_db.get())(dicts, dict_schema, *GetMainDb(db),
                                   {GetFallbackDb(db).get()}));
  EXPECT_EQ(result_slice.size(), 3);
  EXPECT_EQ(result_schema, dict_schema);
  EXPECT_NE(result_slice[0], dicts[0]);
  EXPECT_NE(result_slice[1], dicts[1]);
  EXPECT_NE(result_slice[2], dicts[2]);
  EXPECT_TRUE(result_slice[0].is_dict());
  EXPECT_TRUE(result_slice[1].is_dict());
  EXPECT_TRUE(result_slice[2].is_dict());
  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  auto result_dicts_expanded = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {result_slice[0], result_slice[0], result_slice[0], result_slice[1],
       result_slice[1], result_slice[2], result_slice[2]}));
  ASSERT_OK_AND_ASSIGN((auto [result_keys_multiset, _]),
                       result_db->GetDictKeys(result_slice));
  EXPECT_EQ(result_keys_multiset.present_count(), keys_expanded.size());
  ASSERT_OK_AND_ASSIGN(auto result_key_names,
                       result_db->GetAttr(result_keys_multiset, "name"));
  auto result_keys_builder = arolla::DenseArrayBuilder<ObjectId>(4);
  for (int64_t i = 0; i < result_key_names.size(); ++i) {
    EXPECT_TRUE(result_key_names[i].holds_value<arolla::Bytes>());
    int pos = result_key_names[i].value<arolla::Bytes>().at(0) - 'a';
    EXPECT_TRUE(pos >= 0 && pos < 4);
    EXPECT_TRUE(result_keys_multiset[i].holds_value<ObjectId>());
    result_keys_builder.Set(pos, result_keys_multiset[i].value<ObjectId>());
  }
  auto result_keys =
      DataSliceImpl::Create(std::move(result_keys_builder).Build());
  EXPECT_EQ(result_keys.present_count(), 4);
  auto result_keys_expanded = DataSliceImpl::Create(CreateDenseArray<DataItem>(
      {result_keys[0], result_keys[1], result_keys[2], result_keys[0],
       result_keys[3], result_keys[2], result_keys[3]}));
  ASSERT_OK_AND_ASSIGN(
      auto result_values,
      result_db->GetFromDict(result_dicts_expanded, result_keys_expanded));
  EXPECT_EQ(result_values.present_count(), values.present_count());
  ASSERT_OK_AND_ASSIGN(
      auto result_key_schema,
      result_db->GetSchemaAttr(result_schema, schema::kDictKeysSchemaAttr));
  ASSERT_OK_AND_ASSIGN(
      auto result_value_schema,
      result_db->GetSchemaAttr(result_schema, schema::kDictValuesSchemaAttr));

  ASSERT_OK(expected_db->SetInDict(result_dicts_expanded, result_keys_expanded,
                                   result_values));
  TriplesT resultdata_triples = {{result_keys[0], {{"name", DataItem("a")}}},
                                 {result_keys[1], {{"name", DataItem("b")}}},
                                 {result_keys[2], {{"name", DataItem("c")}}},
                                 {result_keys[3], {{"name", DataItem("d")}}},
                                 {result_values[0], {{"x", DataItem(1)}}},
                                 {result_values[1], {{"x", DataItem(2)}}},
                                 {result_values[2], {{"x", DataItem(3)}}},
                                 {result_values[3], {{"x", DataItem(4)}}},
                                 {result_values[4], {{"x", DataItem(5)}}},
                                 {result_values[5], {{"x", DataItem(6)}}},
                                 {result_values[6], {{"x", DataItem(7)}}}};
  TriplesT expected_schema_triples = {
      {result_schema,
       {{schema::kDictKeysSchemaAttr, result_key_schema},
        {schema::kDictValuesSchemaAttr, result_value_schema}}},
      {result_key_schema, {{"name", DataItem(schema::kText)}}},
      {result_value_schema, {{"x", DataItem(schema::kInt32)}}}};
  SetSchemaTriples(*expected_db, expected_schema_triples);
  SetDataTriples(*expected_db, resultdata_triples);
  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(DeepCloneTest, ObjectsSlice) {
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
      {key_schema, {{"name", DataItem(schema::kText)}}},
      {dict0_schema,
       {{schema::kDictKeysSchemaAttr, DataItem(schema::kText)},
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
  ASSERT_OK_AND_ASSIGN(
      (auto [result_slice, result_schema]),
      DeepCloneOp(result_db.get())(ds, DataItem(schema::kObject),
                                   *GetMainDb(db), {GetFallbackDb(db).get()}));
  auto result_a0 = result_slice[0];
  auto result_a1 = result_slice[1];
  auto result_dict0 = result_slice[5];
  auto result_dict1 = result_slice[6];
  auto result_list0 = result_slice[7];
  auto result_list1 = result_slice[8];
  ASSERT_OK_AND_ASSIGN((auto [result_a2_slice, _]),
                       result_db->GetDictKeys(result_dict1));
  auto result_a2 = result_a2_slice[0];
  ASSERT_OK_AND_ASSIGN(auto result_a3,
                       result_db->GetFromDict(result_dict1, result_a2));
  ASSERT_OK_AND_ASSIGN(auto result_a4, result_db->GetFromList(result_list0, 0));
  ASSERT_OK_AND_ASSIGN(auto result_a5, result_db->GetFromList(result_list0, 1));
  EXPECT_EQ(result_slice.size(), ds.size());
  EXPECT_EQ(result_schema, DataItem(schema::kObject));
  for (int64_t i = 2; i <= 4; ++i) {
    EXPECT_EQ(result_slice[i], ds[i]);
  }
  EXPECT_NE(result_a0, a0);
  EXPECT_NE(result_a1, a1);
  EXPECT_NE(result_dict0, dicts[0]);
  EXPECT_NE(result_dict1, dicts[1]);
  EXPECT_NE(result_list0, lists[0]);
  EXPECT_NE(result_list1, lists[1]);
  ASSERT_OK_AND_ASSIGN(auto result_dict0_schema,
                       result_db->GetAttr(result_dict0, schema::kSchemaAttr));
  ASSERT_OK_AND_ASSIGN(auto result_dict1_schema,
                       result_db->GetAttr(result_dict1, schema::kSchemaAttr));
  ASSERT_OK_AND_ASSIGN(auto result_list0_schema,
                       result_db->GetAttr(result_list0, schema::kSchemaAttr));
  ASSERT_OK_AND_ASSIGN(auto result_list1_schema,
                       result_db->GetAttr(result_list1, schema::kSchemaAttr));
  ASSERT_OK_AND_ASSIGN(auto result_item_schema,
                       result_db->GetAttr(result_a0, schema::kSchemaAttr));
  ASSERT_OK_AND_ASSIGN(auto result_key_schema,
                       result_db->GetSchemaAttr(result_dict1_schema,
                                                schema::kDictKeysSchemaAttr));
  EXPECT_EQ(result_dict0_schema, dict0_schema);
  EXPECT_EQ(result_dict1_schema, dict1_schema);
  EXPECT_EQ(result_list0_schema, list0_schema);
  EXPECT_EQ(result_list1_schema, list1_schema);
  EXPECT_EQ(result_item_schema, item_schema);
  EXPECT_EQ(result_key_schema, key_schema);
  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  ASSERT_OK(expected_db->SetInDict(result_dict0, DataItem("a"), DataItem(1)));
  ASSERT_OK(expected_db->SetInDict(result_dict1, result_a2, result_a3));
  ASSERT_OK(expected_db->ExtendList(
      result_list0, DataSliceImpl::Create(
                        CreateDenseArray<DataItem>({result_a4, result_a5}))));
  ASSERT_OK(expected_db->ExtendList(
      result_list1,
      DataSliceImpl::Create(CreateDenseArray<int32_t>({0, 1, 2}))));
  ;
  TriplesT expected_data_triples = {
      {result_a0,
       {{schema::kSchemaAttr, result_item_schema}, {"x", DataItem(1)}}},
      {result_a1,
       {{schema::kSchemaAttr, result_item_schema}, {"x", DataItem(2)}}},
      {result_a2, {{"name", DataItem("k0")}}},
      {result_a3, {{"x", DataItem(10)}}},
      {result_a4, {{"x", DataItem(3)}}},
      {result_a5, {{"x", DataItem(4)}}},
      {result_dict0, {{schema::kSchemaAttr, result_dict0_schema}}},
      {result_dict1, {{schema::kSchemaAttr, result_dict1_schema}}},
      {result_list0, {{schema::kSchemaAttr, result_list0_schema}}},
      {result_list1, {{schema::kSchemaAttr, result_list1_schema}}},
  };
  TriplesT expected_schema_triples = {
      {result_item_schema, {{"x", DataItem(schema::kInt32)}}},
      {result_key_schema, {{"name", DataItem(schema::kText)}}},
      {result_dict0_schema,
       {{schema::kDictKeysSchemaAttr, DataItem(schema::kText)},
        {schema::kDictValuesSchemaAttr, DataItem(schema::kInt32)}}},
      {result_dict1_schema,
       {{schema::kDictKeysSchemaAttr, result_key_schema},
        {schema::kDictValuesSchemaAttr, result_item_schema}}},
      {result_list0_schema,
       {{schema::kListItemsSchemaAttr, result_item_schema}}},
      {result_list1_schema,
       {{schema::kListItemsSchemaAttr, DataItem(schema::kInt32)}}}};
  SetDataTriples(*expected_db, expected_data_triples);
  SetSchemaTriples(*expected_db, expected_schema_triples);
  EXPECT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

TEST_P(DeepCloneTest, ImplicitSchema) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto obj_ids = DataSliceImpl::AllocateEmptyObjects(3);
  auto schema0 =
      DataItem(CreateUuidWithMainObject<ObjectId::kUuidImplicitSchemaFlag>(
          AllocateSchema().value<ObjectId>(), arolla::Fingerprint(57)));
  auto schema1 =
      DataItem(CreateUuidWithMainObject<ObjectId::kUuidImplicitSchemaFlag>(
          AllocateSchema().value<ObjectId>(), arolla::Fingerprint(58)));
  auto schema2 =
      DataItem(CreateUuidWithMainObject<ObjectId::kUuidImplicitSchemaFlag>(
          AllocateSchema().value<ObjectId>(), arolla::Fingerprint(59)));
  auto a0 = obj_ids[0];
  auto a1 = obj_ids[1];
  auto a2 = obj_ids[2];

  TriplesT data_triples = {{a0,
                            {{schema::kSchemaAttr, schema0},
                             {"x", DataItem(1)},
                             {"y", DataItem(4)}}},
                           {a1,
                            {{schema::kSchemaAttr, schema1},
                             {"x", DataItem(2)},
                             {"y", DataItem(5)}}},
                           {a2,
                            {{schema::kSchemaAttr, schema2},
                             {"x", DataItem(3)},
                             {"y", DataItem(6)}}}};
  TriplesT schema_triples = {
      {schema0,
       {{"x", DataItem(schema::kInt32)}, {"y", DataItem(schema::kInt32)}}},
      {schema1,
       {{"x", DataItem(schema::kInt32)}, {"y", DataItem(schema::kInt32)}}},
      {schema2,
       {{"x", DataItem(schema::kInt32)}, {"y", DataItem(schema::kInt32)}}}};
  SetDataTriples(*db, data_triples);
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto result_db = DataBagImpl::CreateEmptyDatabag();
  ASSERT_OK_AND_ASSIGN(
      (auto [result_slice, result_schema]),
      DeepCloneOp(result_db.get())(obj_ids, DataItem(schema::kObject),
                                   *GetMainDb(db), {GetFallbackDb(db).get()}));
  EXPECT_EQ(result_slice.size(), 3);
  EXPECT_NE(result_slice[0], a0);
  EXPECT_NE(result_slice[1], a1);
  EXPECT_NE(result_slice[2], a2);
  EXPECT_EQ(result_schema, schema::kObject);
  ASSERT_OK_AND_ASSIGN(
      auto result_schema0,
      result_db->GetAttr(result_slice[0], schema::kSchemaAttr));
  ASSERT_OK_AND_ASSIGN(
      auto result_schema1,
      result_db->GetAttr(result_slice[1], schema::kSchemaAttr));
  ASSERT_OK_AND_ASSIGN(
      auto result_schema2,
      result_db->GetAttr(result_slice[2], schema::kSchemaAttr));
  EXPECT_NE(result_schema0, schema0);
  EXPECT_NE(result_schema1, schema1);
  EXPECT_NE(result_schema2, schema2);
  EXPECT_TRUE(result_schema0.holds_value<ObjectId>());
  EXPECT_TRUE(result_schema1.holds_value<ObjectId>());
  EXPECT_TRUE(result_schema2.holds_value<ObjectId>());
  EXPECT_NE(result_schema0, result_schema1);
  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  TriplesT expected_data_triples = {{result_slice[0],
                                     {{schema::kSchemaAttr, result_schema0},
                                      {"x", DataItem(1)},
                                      {"y", DataItem(4)}}},
                                    {result_slice[1],
                                     {{schema::kSchemaAttr, result_schema1},
                                      {"x", DataItem(2)},
                                      {"y", DataItem(5)}}},
                                    {result_slice[2],
                                     {{schema::kSchemaAttr, result_schema2},
                                      {"x", DataItem(3)},
                                      {"y", DataItem(6)}}}};
  TriplesT expected_schema_triples = {
      {result_schema0,
       {{"x", DataItem(schema::kInt32)}, {"y", DataItem(schema::kInt32)}}},
      {result_schema1,
       {{"x", DataItem(schema::kInt32)}, {"y", DataItem(schema::kInt32)}}},
      {result_schema2,
       {{"x", DataItem(schema::kInt32)}, {"y", DataItem(schema::kInt32)}}}};
  SetDataTriples(*expected_db, expected_data_triples);
  SetSchemaTriples(*expected_db, expected_schema_triples);
  ASSERT_NE(result_db.get(), db.get());
  EXPECT_THAT(result_db, DataBagEqual(expected_db));
}

TEST_P(DeepCloneTest, SchemaSlice) {
  auto db = DataBagImpl::CreateEmptyDatabag();
  auto s1 = AllocateSchema();
  auto s2 = AllocateSchema();
  auto s3 = AllocateSchema();
  TriplesT schema_triples = {
      {s1, {{"x", DataItem(schema::kInt32)}, {"y", s3}}},
      {s2, {{"a", DataItem(schema::kText)}}},
      {s3, {{"self", s3}}},
  };
  SetSchemaTriples(*db, schema_triples);
  SetSchemaTriples(*db, GenNoiseSchemaTriples());
  SetDataTriples(*db, GenNoiseDataTriples());

  auto ds = DataSliceImpl::Create(CreateDenseArray<DataItem>({s1, s2}));
  auto result_db = DataBagImpl::CreateEmptyDatabag();
  ASSERT_OK_AND_ASSIGN(
      (auto [result_slice, result_schema]),
      DeepCloneOp(result_db.get())(ds, DataItem(schema::kSchema),
                                   *GetMainDb(db), {GetFallbackDb(db).get()}));
  auto expected_db = DataBagImpl::CreateEmptyDatabag();
  EXPECT_NE(result_db.get(), db.get());
  EXPECT_NE(result_slice[0], s1);
  EXPECT_NE(result_slice[1], s2);
  ASSERT_OK_AND_ASSIGN(auto result_s3,
                       result_db->GetSchemaAttr(result_slice[0], "y"));
  TriplesT expected_schema_triples = {
      {result_slice[0], {{"x", DataItem(schema::kInt32)}, {"y", result_s3}}},
      {result_slice[1], {{"a", DataItem(schema::kText)}}},
      {result_s3, {{"self", result_s3}}},
  };
  SetSchemaTriples(*expected_db, expected_schema_triples);
  EXPECT_THAT(result_db, DataBagEqual(*expected_db));
}

}  // namespace
}  // namespace koladata::internal
