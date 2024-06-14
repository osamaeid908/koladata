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
#include "koladata/object_factories.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "koladata/data_bag.h"
#include "koladata/data_slice.h"
#include "koladata/internal/data_bag.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/data_slice.h"
#include "koladata/internal/dtype.h"
#include "koladata/internal/object_id.h"
#include "koladata/internal/schema_utils.h"
#include "koladata/internal/testing/matchers.h"
#include "koladata/internal/uuid_object.h"
#include "koladata/test_utils.h"
#include "koladata/testing/matchers.h"
#include "koladata/testing/status_matchers_backport.h"
#include "arolla/dense_array/dense_array.h"
#include "arolla/dense_array/edge.h"
#include "arolla/jagged_shape/testing/matchers.h"
#include "arolla/qtype/qtype_traits.h"
#include "arolla/util/text.h"

namespace koladata {
namespace {

using ObjectId = internal::ObjectId;

using ::arolla::CreateDenseArray;
using ::arolla::DenseArrayEdge;
using ::arolla::Text;
using ::arolla::testing::IsEquivalentTo;
using ::koladata::internal::testing::DataItemWith;
using ::koladata::internal::testing::MissingDataItem;
using ::koladata::testing::IsEquivalentTo;
using ::koladata::testing::IsOkAndHolds;
using ::koladata::testing::StatusIs;
using ::testing::ElementsAre;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::IsTrue;
using ::testing::Not;
using ::testing::Property;

TEST(EntitySchemaTest, CreateSchema) {
  auto db = DataBag::Empty();
  auto int_s = test::Schema(schema::kInt32);
  auto float_s = test::Schema(schema::kFloat32);

  ASSERT_OK_AND_ASSIGN(auto entity_schema,
                       CreateEntitySchema(db, {"a", "b"}, {int_s, float_s}));
  EXPECT_EQ(entity_schema.GetSchemaImpl(), schema::kSchema);
  EXPECT_OK(entity_schema.VerifyIsSchema());
  EXPECT_THAT(entity_schema.GetAttr("a"),
              IsOkAndHolds(IsEquivalentTo(int_s.WithDb(db))));
  EXPECT_THAT(entity_schema.GetAttr("b"),
              IsOkAndHolds(IsEquivalentTo(float_s.WithDb(db))));
}

TEST(EntitySchemaTest, Error) {
  auto db = DataBag::Empty();
  auto int_s = test::Schema(schema::kInt32);
  auto non_schema_1 = test::DataItem(42);
  EXPECT_THAT(CreateEntitySchema(db, {"a", "b"}, {int_s, non_schema_1}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("must be SCHEMA, got: INT32")));
  auto non_schema_2 = test::DataSlice<schema::DType>({schema::kInt32});
  EXPECT_THAT(CreateEntitySchema(db, {"a", "b"}, {int_s, non_schema_2}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("can only be 0-rank")));
}

TEST(EntityCreatorTest, DataSlice) {
  constexpr int64_t kSize = 3;
  auto db = DataBag::Empty();

  auto ds_a = test::AllocateDataSlice(kSize, schema::kObject);
  auto ds_b = test::DataSlice<int>({42, std::nullopt, 12});

  ASSERT_OK_AND_ASSIGN(
      auto ds,
      EntityCreator()(db, {std::string("a"), std::string("b")}, {ds_a, ds_b}));
  EXPECT_EQ(ds.GetDb(), db);
  // Schema check.
  EXPECT_TRUE(ds.GetSchemaImpl().value<ObjectId>().IsSchema());
  EXPECT_TRUE(ds.GetSchemaImpl().value<ObjectId>().IsExplicitSchema());

  ASSERT_OK_AND_ASSIGN(auto ds_a_get, db->GetImpl().GetAttr(ds.slice(), "a"));
  EXPECT_EQ(ds_a_get.size(), kSize);
  EXPECT_EQ(ds_a_get.dtype(), arolla::GetQType<ObjectId>());
  EXPECT_EQ(ds_a_get.allocation_ids(), ds_a.slice().allocation_ids());
  EXPECT_EQ(ds_a_get.values<ObjectId>().size(), kSize);
  EXPECT_THAT(ds_a_get.values<ObjectId>(),
              ElementsAreArray(ds_a.slice().values<ObjectId>()));
  // Schema attribute check.
  ASSERT_OK_AND_ASSIGN(auto schema_a_get, ds.GetSchema().GetAttr("a"));
  EXPECT_EQ(schema_a_get.item(), schema::kObject);

  ASSERT_OK_AND_ASSIGN(auto ds_b_get, db->GetImpl().GetAttr(ds.slice(), "b"));
  EXPECT_EQ(ds_b_get.size(), kSize);
  EXPECT_EQ(ds_b_get.dtype(), arolla::GetQType<int>());
  EXPECT_EQ(ds_b_get.allocation_ids().size(), 0);
  EXPECT_EQ(ds_b_get.values<int>().size(), kSize);
  EXPECT_THAT(ds_b_get.values<int>(), ElementsAre(42, std::nullopt, 12));
  // Schema attribute check.
  ASSERT_OK_AND_ASSIGN(auto schema_b_get, ds.GetSchema().GetAttr("b"));
  EXPECT_EQ(schema_b_get.item(), schema::kInt32);
}

TEST(EntityCreatorTest, DataItem) {
  auto db = DataBag::Empty();
  auto ds_a = test::DataItem(internal::AllocateSingleObject());
  auto ds_b = test::DataItem(42);

  ASSERT_OK_AND_ASSIGN(
      auto ds,
      EntityCreator()(db, {std::string("a"), std::string("b")}, {ds_a, ds_b}));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_EQ(ds.size(), 1);
  EXPECT_EQ(ds.GetShape().rank(), 0);
  // Schema check.
  EXPECT_TRUE(ds.GetSchemaImpl().value<ObjectId>().IsSchema());
  EXPECT_TRUE(ds.GetSchemaImpl().value<ObjectId>().IsExplicitSchema());

  ASSERT_OK_AND_ASSIGN(auto ds_a_get, db->GetImpl().GetAttr(ds.item(), "a"));
  EXPECT_EQ(ds_a_get.dtype(), arolla::GetQType<ObjectId>());
  EXPECT_EQ(ds_a_get.value<ObjectId>(), ds_a.item().value<ObjectId>());
  // Schema attribute check.
  ASSERT_OK_AND_ASSIGN(auto schema_a_get, ds.GetSchema().GetAttr("a"));
  EXPECT_EQ(schema_a_get.item(), schema::kObject);

  ASSERT_OK_AND_ASSIGN(auto ds_b_get, db->GetImpl().GetAttr(ds.item(), "b"));
  EXPECT_EQ(ds_b_get.dtype(), arolla::GetQType<int>());
  EXPECT_EQ(ds_b_get.value<int>(), 42);
  // Schema attribute check.
  ASSERT_OK_AND_ASSIGN(auto schema_b_get, ds.GetSchema().GetAttr("b"));
  EXPECT_EQ(schema_b_get.item(), schema::kInt32);
}

TEST(EntityCreatorTest, SchemaArg) {
  auto db = DataBag::Empty();
  auto int_s = test::Schema(schema::kInt32);
  auto text_s = test::Schema(schema::kText);
  auto entity_schema = *CreateEntitySchema(db, {"a", "b"}, {int_s, text_s});

  ASSERT_OK_AND_ASSIGN(
      auto entity,
      EntityCreator()(
          db,
          {"a", "b"}, {test::DataItem(42), test::DataItem("xyz")},
          entity_schema));

  EXPECT_EQ(entity.GetSchemaImpl(), entity_schema.item());
  EXPECT_THAT(entity.GetAttr("a"),
              IsOkAndHolds(IsEquivalentTo(test::DataItem(42).WithDb(db))));
  EXPECT_THAT(entity.GetAttr("b"),
              IsOkAndHolds(IsEquivalentTo(test::DataItem("xyz").WithDb(db))));
}

TEST(EntityCreatorTest, SchemaArg_InvalidSchema) {
  auto db = DataBag::Empty();
  EXPECT_THAT(EntityCreator()(db, {"a", "b"},
                              {test::DataItem(42), test::DataItem("xyz")},
                              test::DataItem(42)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("must be SCHEMA, got: INT32")));
  EXPECT_THAT(EntityCreator()(db, {"a", "b"},
                              {test::DataItem(42), test::DataItem("xyz")},
                              test::Schema(schema::kObject)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("requires Entity schema, got OBJECT")));
}

TEST(EntityCreatorTest, SchemaArg_WithFallback) {
  auto db = DataBag::Empty();
  auto int_s = test::Schema(schema::kInt32);
  auto entity_schema = *CreateEntitySchema(db, {"a"}, {int_s});

  auto fb_db = DataBag::Empty();
  auto text_s = test::Schema(schema::kText);
  ASSERT_OK(entity_schema.WithDb(fb_db).SetAttr("b", text_s));

  entity_schema = entity_schema.WithDb(
      DataBag::ImmutableEmptyWithFallbacks({db, fb_db}));

  auto new_db = DataBag::Empty();
  ASSERT_OK_AND_ASSIGN(
      auto entity,
      EntityCreator()(
          new_db,
          {"a", "b"}, {test::DataItem(42), test::DataItem("xyz")},
          entity_schema));

  EXPECT_EQ(entity.GetSchemaImpl(), entity_schema.item());
  EXPECT_THAT(entity.GetAttr("a"),
              IsOkAndHolds(IsEquivalentTo(test::DataItem(42).WithDb(new_db))));
  EXPECT_THAT(
      entity.GetAttr("b"),
      IsOkAndHolds(IsEquivalentTo(test::DataItem("xyz").WithDb(new_db))));
}

TEST(EntityCreatorTest, SchemaArg_ImplicitCasting) {
  auto db = DataBag::Empty();
  auto int_s = test::Schema(schema::kFloat32);
  auto entity_schema = *CreateEntitySchema(db, {"a"}, {int_s});

  auto ds_a = test::DataItem(42);
  ASSERT_EQ(ds_a.GetSchemaImpl(), schema::kInt32);

  ASSERT_OK_AND_ASSIGN(auto entity,
                       EntityCreator()(db, {"a"}, {ds_a}, entity_schema));

  EXPECT_THAT(
      entity.GetAttr("a"),
      IsOkAndHolds(
          AllOf(IsEquivalentTo(test::DataItem(42.0f).WithDb(db)),
                Property(&DataSlice::GetSchemaImpl, Eq(schema::kFloat32)))));
}

TEST(EntityCreatorTest, SchemaArg_CastingFails) {
  auto db = DataBag::Empty();
  auto int_s = test::Schema(schema::kFloat32);
  auto entity_schema = *CreateEntitySchema(db, {"a"}, {int_s});

  EXPECT_THAT(
      EntityCreator()(db, {"a"}, {test::DataItem("xyz")}, entity_schema),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("schema for attribute 'a' is incompatible")));
}

TEST(EntityCreatorTest, SchemaArg_UpdateSchema) {
  auto db = DataBag::Empty();
  auto int_s = test::Schema(schema::kFloat32);
  auto entity_schema = *CreateEntitySchema(db, {"a"}, {int_s});

  EXPECT_THAT(
      EntityCreator()(db, {"a", "b"},
                      {test::DataItem(42), test::DataItem("xyz")},
                      entity_schema),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("attribute 'b' is missing on the schema")));

  ASSERT_OK_AND_ASSIGN(
      auto entity,
      EntityCreator()(db, {"a", "b"},
                      {test::DataItem(42), test::DataItem("xyz")},
                      entity_schema, /*update_schema=*/true));

  EXPECT_THAT(entity.GetAttr("b"),
              IsOkAndHolds(IsEquivalentTo(test::DataItem("xyz").WithDb(db))));
}

TEST(EntityCreatorTest, SchemaArg_NoDb) {
  auto schema_db = DataBag::Empty();
  auto int_s = test::Schema(schema::kInt32);
  auto entity_schema = *CreateEntitySchema(schema_db, {"a"}, {int_s});

  auto db = DataBag::Empty();
  EXPECT_THAT(EntityCreator()(db, {"a"}, {test::DataItem(42)},
                              entity_schema.WithDb(nullptr)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("attribute 'a' is missing on the schema")));

  ASSERT_OK_AND_ASSIGN(auto entity,
                       EntityCreator()(db, {"a"}, {test::DataItem(42)},
                                       entity_schema.WithDb(nullptr),
                                       /*update_schema=*/true));
  EXPECT_THAT(entity.GetAttr("a"),
              IsOkAndHolds(IsEquivalentTo(test::DataItem(42).WithDb(db))));
}

TEST(EntityCreatorTest, SchemaArg_Any) {
  auto db = DataBag::Empty();
  ASSERT_OK_AND_ASSIGN(auto entity,
                       EntityCreator()(db, {"a"}, {test::DataItem(42)},
                                       test::Schema(schema::kAny)));

  EXPECT_THAT(entity.GetAttr("a"),
              IsOkAndHolds(
                  IsEquivalentTo(test::DataItem(42, schema::kAny).WithDb(db))));
}

TEST(EntityCreatorTest, PrimitiveToEntity) {
  auto db = DataBag::Empty();
  EXPECT_THAT(
      EntityCreator()(db, test::DataSlice<int>({1, 2, 3})),
      IsOkAndHolds(
          AllOf(Property(&DataSlice::slice, ElementsAre(1, 2, 3)),
                Property(&DataSlice::GetSchemaImpl, Eq(schema::kInt32)),
                Property(&DataSlice::GetDb, Eq(db)))));
  EXPECT_THAT(
      EntityCreator()(db, test::DataItem(42)),
      IsOkAndHolds(
          AllOf(Property(&DataSlice::item, Eq(42)),
                Property(&DataSlice::GetSchemaImpl, Eq(schema::kInt32)),
                Property(&DataSlice::GetDb, Eq(db)))));
}

TEST(EntityCreatorTest, EntityToEntity) {
  auto db_val = DataBag::Empty();
  auto db = DataBag::Empty();
  ASSERT_OK_AND_ASSIGN(
      auto entity_val,
      EntityCreator()(db_val, {std::string("a")}, {test::DataItem(42)}));

  // NOTE: The caller must take care of proper adoption of `value` DataBag.
  ASSERT_OK_AND_ASSIGN(internal::DataBagImpl& db_impl, db->GetMutableImpl());
  ASSERT_OK(db_impl.MergeInplace(db_val->GetImpl()));

  ASSERT_OK_AND_ASSIGN(auto entity, EntityCreator()(db, entity_val));
  EXPECT_EQ(entity.item(), entity_val.item());
  EXPECT_EQ(entity.GetSchemaImpl(), entity_val.GetSchemaImpl());
}

TEST(ObjectCreatorTest, ObjectToEntity) {
  auto db_val = DataBag::Empty();
  auto db = DataBag::Empty();
  EXPECT_THAT(
      EntityCreator()(db, *ObjectCreator()(db_val, test::DataSlice<int>({1}))),
      IsOkAndHolds(
          AllOf(Property(&DataSlice::slice, ElementsAre(1)),
                Property(&DataSlice::GetSchemaImpl, Eq(schema::kObject)),
                Property(&DataSlice::GetDb, Eq(db)))));
}

TEST(ObjectCreatorTest, DataSlice) {
  constexpr int64_t kSize = 3;
  auto db = DataBag::Empty();

  auto ds_a = test::AllocateDataSlice(3, schema::kObject);
  auto ds_b = test::DataSlice<int>({42, std::nullopt, 12});

  ASSERT_OK_AND_ASSIGN(
      auto ds,
      ObjectCreator()(db, {std::string("a"), std::string("b")}, {ds_a, ds_b}));
  EXPECT_EQ(ds.GetDb(), db);
  // Implicit schema stored in __schema__ "normal" attribute.
  EXPECT_EQ(ds.GetSchemaImpl(), schema::kObject);
  ASSERT_OK_AND_ASSIGN(auto schema_slice,
                       db->GetImpl().GetAttr(ds.slice(), schema::kSchemaAttr));
  schema_slice.values<ObjectId>().ForEach(
      [&](int64_t id, bool present, ObjectId schema_id) {
        ASSERT_TRUE(present);
        EXPECT_TRUE(schema_id.IsImplicitSchema());
        EXPECT_TRUE(schema_id.IsUuid());
      });

  ASSERT_OK_AND_ASSIGN(auto ds_a_get, db->GetImpl().GetAttr(ds.slice(), "a"));
  EXPECT_EQ(ds_a_get.size(), kSize);
  EXPECT_EQ(ds_a_get.dtype(), arolla::GetQType<ObjectId>());
  EXPECT_EQ(ds_a_get.allocation_ids(), ds_a.slice().allocation_ids());
  EXPECT_EQ(ds_a_get.values<ObjectId>().size(), kSize);
  EXPECT_THAT(ds_a_get, IsEquivalentTo(ds_a.slice()));
  // Schema attribute check.
  EXPECT_THAT(
      db->GetImpl().GetSchemaAttr(schema_slice, "a"),
      IsOkAndHolds(ElementsAre(schema::kObject, schema::kObject,
                               schema::kObject)));

  ASSERT_OK_AND_ASSIGN(auto ds_b_get, db->GetImpl().GetAttr(ds.slice(), "b"));
  EXPECT_EQ(ds_b_get.size(), kSize);
  EXPECT_EQ(ds_b_get.dtype(), arolla::GetQType<int>());
  EXPECT_EQ(ds_b_get.allocation_ids().size(), 0);
  EXPECT_EQ(ds_b_get.values<int>().size(), kSize);
  EXPECT_THAT(ds_b_get.values<int>(), ElementsAre(42, std::nullopt, 12));
  // Schema attribute check.
  EXPECT_THAT(
      db->GetImpl().GetSchemaAttr(schema_slice, "b"),
      IsOkAndHolds(
          ElementsAre(schema::kInt32, schema::kInt32, schema::kInt32)));
}

TEST(ObjectCreatorTest, DataItem) {
  auto db = DataBag::Empty();
  auto ds_a = test::DataItem(internal::AllocateSingleObject());
  auto ds_b = test::DataItem(42);

  ASSERT_OK_AND_ASSIGN(
      auto ds,
      ObjectCreator()(db, {std::string("a"), std::string("b")}, {ds_a, ds_b}));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_EQ(ds.size(), 1);
  EXPECT_EQ(ds.GetShape().rank(), 0);
  // Implicit schema stored in __schema__ "normal" attribute.
  EXPECT_EQ(ds.GetSchemaImpl(), schema::kObject);
  ASSERT_OK_AND_ASSIGN(auto schema_item,
                       db->GetImpl().GetAttr(ds.item(), schema::kSchemaAttr));
  EXPECT_TRUE(schema_item.value<ObjectId>().IsImplicitSchema());
  EXPECT_TRUE(schema_item.value<ObjectId>().IsUuid());

  ASSERT_OK_AND_ASSIGN(auto ds_a_get, db->GetImpl().GetAttr(ds.item(), "a"));
  EXPECT_EQ(ds_a_get.dtype(), arolla::GetQType<ObjectId>());
  EXPECT_EQ(ds_a_get.value<ObjectId>(), ds_a.item().value<ObjectId>());
  // Schema attribute check.
  EXPECT_THAT(db->GetImpl().GetSchemaAttr(schema_item, "a"),
              IsOkAndHolds(schema::kObject));

  ASSERT_OK_AND_ASSIGN(auto ds_b_get, db->GetImpl().GetAttr(ds.item(), "b"));
  EXPECT_EQ(ds_b_get.dtype(), arolla::GetQType<int>());
  EXPECT_EQ(ds_b_get.value<int>(), 42);
  // Schema attribute check.
  EXPECT_THAT(db->GetImpl().GetSchemaAttr(schema_item, "b"),
              IsOkAndHolds(schema::kInt32));
}

TEST(ObjectCreatorTest, InvalidSchemaArg) {
  auto db = DataBag::Empty();
  auto ds_a = test::DataItem(42);
  auto entity_schema = test::Schema(schema::kAny);
  EXPECT_THAT(ObjectCreator()(db, {"a", "schema"}, {ds_a, entity_schema}),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("please use new_...() instead of obj_...()")));
}

TEST(ObjectCreatorTest, PrimitiveToObject) {
  auto db = DataBag::Empty();
  EXPECT_THAT(
      ObjectCreator()(db, test::DataSlice<int>({1, 2, 3})),
      IsOkAndHolds(
          AllOf(Property(&DataSlice::slice, ElementsAre(1, 2, 3)),
                Property(&DataSlice::GetSchemaImpl, Eq(schema::kObject)),
                Property(&DataSlice::GetDb, Eq(db)))));
  EXPECT_THAT(
      ObjectCreator()(db, test::DataItem(42)),
      IsOkAndHolds(
          AllOf(Property(&DataSlice::item, Eq(42)),
                Property(&DataSlice::GetSchemaImpl, Eq(schema::kObject)),
                Property(&DataSlice::GetDb, Eq(db)))));
}

TEST(ObjectCreatorTest, EntityToObject) {
  auto db_val = DataBag::Empty();
  auto db = DataBag::Empty();
  ASSERT_OK_AND_ASSIGN(
      auto entity,
      EntityCreator()(db_val, {std::string("a")}, {test::DataItem(42)}));

  // NOTE: The caller must take care of proper adoption of `value` DataBag.
  ASSERT_OK_AND_ASSIGN(internal::DataBagImpl& db_impl, db->GetMutableImpl());
  ASSERT_OK(db_impl.MergeInplace(db_val->GetImpl()));

  ASSERT_OK_AND_ASSIGN(auto obj, ObjectCreator()(db, entity));
  EXPECT_EQ(obj.item(), entity.item());
  EXPECT_EQ(obj.GetSchemaImpl(), schema::kObject);

  EXPECT_THAT(
      obj.GetAttr("__schema__"),
      IsOkAndHolds(
          AllOf(Property(&DataSlice::item, Eq(entity.GetSchemaImpl())),
                Property(&DataSlice::GetSchemaImpl, Eq(schema::kSchema)))));
  EXPECT_THAT(
      obj.GetAttr("a"),
      IsOkAndHolds(
          AllOf(Property(&DataSlice::item, Eq(42)),
                Property(&DataSlice::GetSchemaImpl, Eq(schema::kInt32)))));
}

TEST(ObjectCreatorTest, ObjectToObject) {
  auto db_val = DataBag::Empty();
  auto db = DataBag::Empty();
  EXPECT_THAT(
      ObjectCreator()(db, *ObjectCreator()(db_val, test::DataSlice<int>({1}))),
      IsOkAndHolds(
          AllOf(Property(&DataSlice::slice, ElementsAre(1)),
                Property(&DataSlice::GetSchemaImpl, Eq(schema::kObject)),
                Property(&DataSlice::GetDb, Eq(db)))));
}

TEST(ObjectCreatorTest, ObjectConverterError) {
  auto db = DataBag::Empty();
  EXPECT_THAT(ObjectCreator()(db, test::DataSlice<int>({1}, schema::kAny)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "schema embedding is only supported for primitive and "
                       "entity schemas, got ANY"));
}

TEST(UuObjectCreatorTest, DataSlice) {
  auto db = DataBag::Empty();

  auto ds_a = test::AllocateDataSlice(3, schema::kObject);
  auto ds_b = test::DataSlice<int>({42, std::nullopt, 12});

  ASSERT_OK_AND_ASSIGN(
      auto ds,
      UuObjectCreator()(
          db, "", {std::string("a"), std::string("b")}, {ds_a, ds_b}));
  EXPECT_EQ(ds.GetDb(), db);
  // Implicit schema stored in __schema__ "normal" attribute.
  EXPECT_EQ(ds.GetSchemaImpl(), schema::kObject);
  ds.slice().values<ObjectId>().ForEach(
      [&](int64_t id, bool present, ObjectId object_id) {
        EXPECT_TRUE(object_id.IsUuid());
      });

  ASSERT_OK_AND_ASSIGN(auto ds_a_get, ds.GetAttr("a"));
  EXPECT_THAT(ds_a_get.slice(), IsEquivalentTo(ds_a.slice()));
  // Schema attribute check.
  EXPECT_EQ(ds_a_get.GetSchemaImpl(), schema::kObject);

  ASSERT_OK_AND_ASSIGN(auto ds_b_get, ds.GetAttr("b"));
  EXPECT_THAT(ds_b_get.slice(), IsEquivalentTo(ds_b.slice()));
  // Schema attribute check.
  EXPECT_EQ(ds_b_get.GetSchemaImpl(), schema::kInt32);

  // Different objects have different uuids.
  ASSERT_OK_AND_ASSIGN(
      auto ds_2,
      UuObjectCreator()(
          db, "", {std::string("a"), std::string("b")}, {ds_b, ds_a}));
  EXPECT_THAT(ds.slice(), Not(IsEquivalentTo(ds_2.slice())));
  // Different seeds lead to different uuids.
  ASSERT_OK_AND_ASSIGN(
      auto ds_3,
      UuObjectCreator()(
          db, "seed", {std::string("a"), std::string("b")}, {ds_a, ds_b}));
  EXPECT_THAT(ds.slice(), Not(IsEquivalentTo(ds_3.slice())));
}

TEST(UuObjectCreatorTest, DataItem) {
  auto db = DataBag::Empty();
  auto ds_a = test::DataItem(internal::AllocateSingleObject());
  auto ds_b = test::DataItem(42);

  ASSERT_OK_AND_ASSIGN(
      auto ds,
      UuObjectCreator()(
          db, "" , {std::string("a"), std::string("b")}, {ds_a, ds_b}));
  EXPECT_TRUE(ds.item().value<ObjectId>().IsUuid());
  ASSERT_OK_AND_ASSIGN(auto ds_a_get, ds.GetAttr("a"));
  EXPECT_THAT(ds_a_get.item(), IsEquivalentTo(ds_a.item()));
  // Schema attribute check.
  EXPECT_EQ(ds_a_get.GetSchemaImpl(), schema::kObject);

  ASSERT_OK_AND_ASSIGN(auto ds_b_get, ds.GetAttr("b"));
  EXPECT_THAT(ds_b_get.item(), IsEquivalentTo(ds_b.item()));
  // Schema attribute check.
  EXPECT_EQ(ds_b_get.GetSchemaImpl(), schema::kInt32);

  // Different objects have different uuids.
  ASSERT_OK_AND_ASSIGN(
      auto ds_2,
      UuObjectCreator()(
          db, "", {std::string("a"), std::string("b")}, {ds_b, ds_a}));
  EXPECT_THAT(ds.item(), Not(IsEquivalentTo(ds_2.item())));
  // Different seeds lead to different uuids.
  ASSERT_OK_AND_ASSIGN(
      auto ds_3,
      UuObjectCreator()(
          db, "seed", {std::string("a"), std::string("b")}, {ds_a, ds_b}));
  EXPECT_THAT(ds.item(), Not(IsEquivalentTo(ds_3.item())));
}

TEST(UuObjectCreatorTest, Empty) {
  auto db = DataBag::Empty();
  ASSERT_OK_AND_ASSIGN(auto ds_1, UuObjectCreator()(db, "seed1", {}, {}));
  EXPECT_TRUE(ds_1.item().value<ObjectId>().IsUuid());
  ASSERT_OK_AND_ASSIGN(auto ds_2, UuObjectCreator()(db, "seed2", {}, {}));
  EXPECT_TRUE(ds_2.item().value<ObjectId>().IsUuid());
  EXPECT_THAT(ds_1.item(), Not(IsEquivalentTo(ds_2.item())));
  ASSERT_OK_AND_ASSIGN(auto ds_3, UuObjectCreator()(db, "seed1", {}, {}));
  EXPECT_THAT(ds_1.item(), IsEquivalentTo(ds_3.item()));
}

TEST(UuObjectCreatorTest, UuObjectCreationAfterModification) {
  auto db = DataBag::Empty();
  auto ds_a = test::DataItem(2);
  auto ds_b = test::DataItem(3);
  auto ds_c = test::DataItem(4);

  ASSERT_OK_AND_ASSIGN(
      auto ds,
      UuObjectCreator()(
          db, "" , {std::string("a"), std::string("b")}, {ds_a, ds_b}));

  ASSERT_OK(ds.SetAttr("c", ds_c));

  ASSERT_OK_AND_ASSIGN(
      auto new_ds_fetch,
      UuObjectCreator()(
          db, "" , {std::string("a"), std::string("b")}, {ds_a, ds_b}));

  ASSERT_OK_AND_ASSIGN(auto ds_c_get, new_ds_fetch.GetAttr("c"));
  EXPECT_THAT(ds_c_get.item(), IsEquivalentTo(ds_c.item()));
}

template <typename Creator>
class CreatorTest : public ::testing::Test {
 public:
  using CreatorT = Creator;

  static void VerifyDataSliceSchema(const DataBag& db, const DataSlice& ds) {
    if constexpr (std::is_same_v<Creator, EntityCreator>) {
      // Verify schema is explicit in case of Entities.
      EXPECT_TRUE(ds.GetSchemaImpl().value<ObjectId>().IsExplicitSchema());
    } else {
      EXPECT_EQ(ds.GetSchemaImpl(), schema::kObject);
      ds.VisitImpl([&]<class ImplT>(const ImplT& impl) {
        ASSERT_OK_AND_ASSIGN(auto schema_attr,
                             db.GetImpl().GetAttr(impl, schema::kSchemaAttr));
        // Verify __schema__ attribute contains implicit schemas in case of
        // Objects.
        if constexpr (std::is_same_v<ImplT, internal::DataItem>) {
          EXPECT_TRUE(
              schema_attr.template value<ObjectId>().IsImplicitSchema());
          EXPECT_TRUE(
              schema_attr.template value<ObjectId>().IsUuid());
        } else {
          schema_attr.template values<ObjectId>().ForEachPresent(
              [&](int64_t /*id*/, ObjectId schema_id) {
                EXPECT_TRUE(schema_id.IsImplicitSchema());
                EXPECT_TRUE(schema_id.IsUuid());
              });
        }
      });
    }
  }
};

using CreatorTestTypes = ::testing::Types<EntityCreator, ObjectCreator>;
TYPED_TEST_SUITE(CreatorTest, CreatorTestTypes);

TYPED_TEST(CreatorTest, NoInputs) {
  typename TestFixture::CreatorT creator;
  auto db = DataBag::Empty();

  ASSERT_OK_AND_ASSIGN(auto ds, creator(db, {}, {}));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_EQ(ds.GetShape().rank(), 0);
  TestFixture::VerifyDataSliceSchema(*db, ds);
}

TYPED_TEST(CreatorTest, Shaped) {
  typename TestFixture::CreatorT creator;
  auto shape = DataSlice::JaggedShape::FlatFromSize(3);
  auto db = DataBag::Empty();

  ASSERT_OK_AND_ASSIGN(auto ds, creator(db, shape));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(shape));
  TestFixture::VerifyDataSliceSchema(*db, ds);
}

TYPED_TEST(CreatorTest, AutoBroadcasting) {
  typename TestFixture::CreatorT creator;
  constexpr int64_t kSize = 3;
  auto db = DataBag::Empty();
  auto ds_a = test::AllocateDataSlice(kSize, schema::kObject);
  auto ds_b = test::DataItem(internal::AllocateSingleObject());

  ASSERT_OK_AND_ASSIGN(auto ds, creator(
      db, {std::string("a"), std::string("b")}, {ds_a, ds_b}));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(ds_a.GetShape()));
  EXPECT_TRUE(ds_b.GetShape().IsBroadcastableTo(ds.GetShape()));
  TestFixture::VerifyDataSliceSchema(*db, ds);

  ASSERT_OK_AND_ASSIGN(auto ds_b_get, db->GetImpl().GetAttr(ds.slice(), "b"));
  auto obj_id = ds_b.item().value<ObjectId>();
  EXPECT_THAT(ds_b_get.template values<ObjectId>(),
              ElementsAre(obj_id, obj_id, obj_id));

  ds_b = test::AllocateDataSlice(2, schema::kObject);
  EXPECT_THAT(
      creator(db, {std::string("a"), std::string("b")}, {ds_a, ds_b}),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("shapes are not compatible")));
}

TEST(ObjectFactoriesTest, CreateEmptyList) {
  auto db = DataBag::Empty();

  {
    // Scalar, no schema.
    ASSERT_OK_AND_ASSIGN(auto ds,
                         CreateEmptyList(db, /*item_schema=*/std::nullopt));
    EXPECT_THAT(ds.item(),
                DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue())));
    EXPECT_EQ(ds.GetDb(), db);
    EXPECT_EQ(ds.GetShape().rank(), 0);
    EXPECT_THAT(ds.GetSchema().GetAttr("__items__"),
                IsOkAndHolds(Property(&DataSlice::item, schema::kObject)));
  }
  {
    // Scalar, int32 schema.
    ASSERT_OK_AND_ASSIGN(auto ds,
                         CreateEmptyList(db, test::Schema(schema::kInt32)));
    EXPECT_THAT(ds.item(),
                DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue())));
    EXPECT_EQ(ds.GetDb(), db);
    EXPECT_EQ(ds.GetShape().rank(), 0);
    EXPECT_THAT(ds.GetSchema().GetAttr("__items__"),
                IsOkAndHolds(Property(&DataSlice::item, schema::kInt32)));
  }
}

TEST(ObjectFactoriesTest, CreateListsFromLastDimension) {
  auto db = DataBag::Empty();

  {
    // Scalar, deduce schema from values.
    auto shape = DataSlice::JaggedShape::FlatFromSize(3);
    auto values = test::DataSlice<int32_t>({1, 2, 3}, shape, db);
    ASSERT_OK_AND_ASSIGN(
        auto ds, CreateListsFromLastDimension(db, values,
                                              /*item_schema=*/std::nullopt));
    EXPECT_THAT(ds.item(),
                DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue())));
    EXPECT_EQ(ds.GetDb(), db);
    EXPECT_EQ(ds.GetShape().rank(), 0);
    EXPECT_THAT(ds.ExplodeList(0, std::nullopt),
                IsOkAndHolds(Property(&DataSlice::slice,
                                      ElementsAre(DataItemWith<int32_t>(1),
                                                  DataItemWith<int32_t>(2),
                                                  DataItemWith<int32_t>(3)))));
    EXPECT_THAT(ds.GetSchema().GetAttr("__items__"),
                IsOkAndHolds(Property(&DataSlice::item, schema::kInt32)));
  }
  {
    // Scalar, int32 values, int64 schema.
    auto shape = DataSlice::JaggedShape::FlatFromSize(3);
    auto values = test::DataSlice<int32_t>({1, 2, 3}, shape, db);
    EXPECT_EQ(values.GetSchemaImpl(), schema::kInt32);
    ASSERT_OK_AND_ASSIGN(
        auto ds,
        CreateListsFromLastDimension(db, values, test::Schema(schema::kInt64)));
    EXPECT_THAT(ds.item(),
                DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue())));
    EXPECT_EQ(ds.GetDb(), db);
    EXPECT_EQ(ds.GetShape().rank(), 0);
    EXPECT_THAT(ds.ExplodeList(0, std::nullopt),
                IsOkAndHolds(Property(&DataSlice::slice,
                                      ElementsAre(DataItemWith<int64_t>(1),
                                                  DataItemWith<int64_t>(2),
                                                  DataItemWith<int64_t>(3)))));
    EXPECT_THAT(ds.GetSchema().GetAttr("__items__"),
                IsOkAndHolds(Property(&DataSlice::item, schema::kInt64)));
  }
  {
    // Scalar, text values, int32 schema.
    auto shape = DataSlice::JaggedShape::FlatFromSize(3);
    auto values =
        test::DataSlice<arolla::Text>({"foo", "bar", "baz"}, shape, db);
    EXPECT_EQ(values.GetSchemaImpl(), schema::kText);
    EXPECT_THAT(
        CreateListsFromLastDimension(db, values, test::Schema(schema::kInt32)),
        StatusIs(absl::StatusCode::kInvalidArgument,
                 HasSubstr("The schema for List Items is incompatible")));
  }
}

TEST(ObjectFactoriesTest, CreateListsFromLastDimension_FromDataSlice) {
  auto db = DataBag::Empty();
  ASSERT_OK_AND_ASSIGN(auto edge, DenseArrayEdge::FromSplitPoints(
                                      CreateDenseArray<int64_t>({0, 3, 5})));
  ASSERT_OK_AND_ASSIGN(
      auto shape, DataSlice::JaggedShape::FlatFromSize(2)->AddDims({edge}));
  auto values = test::DataSlice<int32_t>({1, 2, 3, 4, 5}, shape, db);

  {
    // int32 values, int32 schema.
    ASSERT_OK_AND_ASSIGN(
        auto lists,
        CreateListsFromLastDimension(db, values, test::Schema(schema::kInt32)));

    EXPECT_EQ(lists.GetShape().rank(), 1);
    ASSERT_OK_AND_ASSIGN(auto exploded_lists,
                         lists.ExplodeList(0, std::nullopt));
    EXPECT_THAT(exploded_lists.GetSchemaImpl(), Eq(schema::kInt32));
    EXPECT_THAT(values.slice(), IsEquivalentTo(exploded_lists.slice()));
    EXPECT_THAT(values.GetShape(), IsEquivalentTo(exploded_lists.GetShape()));
  }
  {
    // int32 values, int64 schema.
    ASSERT_OK_AND_ASSIGN(
        auto lists,
        CreateListsFromLastDimension(db, values, test::Schema(schema::kInt64)));

    EXPECT_EQ(lists.GetShape().rank(), 1);
    ASSERT_OK_AND_ASSIGN(auto exploded_lists,
                         lists.ExplodeList(0, std::nullopt));
    EXPECT_THAT(exploded_lists.slice(),
                ElementsAre(DataItemWith<int64_t>(1), DataItemWith<int64_t>(2),
                            DataItemWith<int64_t>(3), DataItemWith<int64_t>(4),
                            DataItemWith<int64_t>(5)));
    EXPECT_THAT(values.GetShape(), IsEquivalentTo(exploded_lists.GetShape()));
  }
  {
    // int32 values, object schema.
    ASSERT_OK_AND_ASSIGN(auto lists,
                         CreateListsFromLastDimension(
                             db, values, test::Schema(schema::kObject)));

    EXPECT_EQ(lists.GetShape().rank(), 1);
    ASSERT_OK_AND_ASSIGN(auto exploded_lists,
                         lists.ExplodeList(0, std::nullopt));
    EXPECT_THAT(values.slice(), IsEquivalentTo(exploded_lists.slice()));
    EXPECT_THAT(values.GetShape(), IsEquivalentTo(exploded_lists.GetShape()));
  }
  {
    // int32 values, no schema.
    ASSERT_OK_AND_ASSIGN(
        auto lists, CreateListsFromLastDimension(db, values,
                                                 /*item_schema=*/std::nullopt));

    EXPECT_EQ(lists.GetShape().rank(), 1);
    ASSERT_OK_AND_ASSIGN(auto exploded_lists,
                         lists.ExplodeList(0, std::nullopt));
    EXPECT_THAT(exploded_lists.GetSchemaImpl(), Eq(schema::kInt32));
    EXPECT_THAT(exploded_lists.slice(), ElementsAreArray(values.slice()));
    EXPECT_THAT(values.GetShape(), IsEquivalentTo(exploded_lists.GetShape()));
  }
  {
    EXPECT_THAT(
        CreateListsFromLastDimension(db, values,
                                     test::Schema(schema::kText)),
        StatusIs(absl::StatusCode::kInvalidArgument,
                 HasSubstr("The schema for List Items is incompatible.")));
  }
  {
    EXPECT_THAT(
        CreateListsFromLastDimension(db, test::DataItem(57),
                                     test::Schema(schema::kFloat32)),
        StatusIs(absl::StatusCode::kInvalidArgument,
                 HasSubstr("creating a list from values requires at least one "
                           "dimension")));
  }
}

TEST(ObjectFactoriesTest, CreateListShaped) {
  auto shape = DataSlice::JaggedShape::FlatFromSize(3);
  auto db = DataBag::Empty();
  ASSERT_OK_AND_ASSIGN(auto ds,
                       CreateListShaped(db, shape, /*values=*/std::nullopt,
                                        test::Schema(schema::kInt32)));
  EXPECT_THAT(
      ds.slice(),
      ElementsAre(
          DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue())),
          DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue())),
          DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue()))));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(shape));
  EXPECT_THAT(ds.GetSchema().GetAttr("__items__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kInt32)));
}

TEST(ObjectFactoriesTest, CreateListShaped_WithValues) {
  auto db = DataBag::Empty();

  ASSERT_OK_AND_ASSIGN(auto edge, DenseArrayEdge::FromSplitPoints(
                                      CreateDenseArray<int64_t>({0, 1, 3})));
  auto shape = DataSlice::JaggedShape::FlatFromSize(2);
  ASSERT_OK_AND_ASSIGN(auto values_shape, shape->AddDims({edge}));
  auto values = test::DataSlice<int>({1, 2, 3}, values_shape, db);

  ASSERT_OK_AND_ASSIGN(auto ds, CreateListShaped(db, shape, values,
                                                 /*item_schema=*/std::nullopt));
  EXPECT_THAT(
      ds.slice(),
      ElementsAre(
          DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue())),
          DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue()))));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(shape));
  // Deduced from values.
  EXPECT_THAT(ds.GetSchema().GetAttr("__items__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kInt32)));
  EXPECT_THAT(ds.ExplodeList(0, std::nullopt),
              IsOkAndHolds(Property(&DataSlice::slice, ElementsAre(1, 2, 3))));
}

TEST(ObjectFactoriesTest, CreateNestedList) {
  auto db = DataBag::Empty();
  ASSERT_OK_AND_ASSIGN(auto edge, DenseArrayEdge::FromSplitPoints(
                                      CreateDenseArray<int64_t>({0, 3, 5})));
  ASSERT_OK_AND_ASSIGN(
      auto shape, DataSlice::JaggedShape::FlatFromSize(2)->AddDims({edge}));
  auto values = test::DataSlice<int>({1, 2, 3, 4, 5}, shape, db);

  {
    ASSERT_OK_AND_ASSIGN(
        auto lists, CreateNestedList(db, values, test::Schema(schema::kInt32)));

    EXPECT_EQ(lists.GetShape().rank(), 0);

    ASSERT_OK_AND_ASSIGN(auto item_schema,
                         lists.GetSchema().GetAttr("__items__"));
    ASSERT_OK_AND_ASSIGN(item_schema, item_schema.GetAttr("__items__"));
    EXPECT_EQ(item_schema.item(), schema::kInt32);

    ASSERT_OK_AND_ASSIGN(auto exploded_lists,
                         lists.ExplodeList(0, std::nullopt));
    ASSERT_OK_AND_ASSIGN(exploded_lists,
                         exploded_lists.ExplodeList(0, std::nullopt));
    EXPECT_EQ(exploded_lists.GetSchemaImpl(), schema::kInt32);
    EXPECT_THAT(values.slice(), IsEquivalentTo(exploded_lists.slice()));
    EXPECT_THAT(values.GetShape(), IsEquivalentTo(exploded_lists.GetShape()));
  }
  {
    EXPECT_THAT(
        CreateNestedList(db, values, test::Schema(schema::kText)),
        StatusIs(absl::StatusCode::kInvalidArgument,
                 HasSubstr("The schema for List Items is incompatible.")));
  }
}

TEST(ObjectFactoriesTest, CreateDictShaped) {
  auto shape = DataSlice::JaggedShape::FlatFromSize(3);
  auto db = DataBag::Empty();

  ASSERT_OK_AND_ASSIGN(
      auto ds, CreateDictShaped(
                   db, shape, /*keys=*/std::nullopt, /*values=*/std::nullopt,
                   /*key_schema=*/std::nullopt, /*value_schema=*/std::nullopt));
  EXPECT_THAT(
      ds.slice(),
      ElementsAre(
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue())),
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue())),
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue()))));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(shape));
  EXPECT_THAT(ds.GetSchema().GetAttr("__keys__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kObject)));
  EXPECT_THAT(ds.GetSchema().GetAttr("__values__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kObject)));
}

TEST(ObjectFactoriesTest, CreateDictShaped_WithValues) {
  auto shape = DataSlice::JaggedShape::FlatFromSize(3);
  auto db = DataBag::Empty();
  ASSERT_OK_AND_ASSIGN(
      auto ds, CreateDictShaped(
                   db, shape, /*keys=*/test::DataSlice<int32_t>({1, 2, 3}),
                   /*values=*/test::DataSlice<int64_t>({57, 58, 59}),
                   /*key_schema=*/std::nullopt, /*value_schema=*/std::nullopt));
  EXPECT_THAT(
      ds.slice(),
      ElementsAre(
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue())),
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue())),
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue()))));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(shape));
  EXPECT_THAT(ds.GetSchema().GetAttr("__keys__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kInt32)));
  EXPECT_THAT(ds.GetSchema().GetAttr("__values__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kInt64)));
  ASSERT_OK_AND_ASSIGN(
      auto expected_keys_shape,
      shape->AddDims({DenseArrayEdge::FromUniformGroups(3, 1).value()}));
  EXPECT_THAT(
      ds.GetDictKeys(),
      IsOkAndHolds(AllOf(Property(&DataSlice::slice,
                                  ElementsAre(DataItemWith<int32_t>(1), 2, 3)),
                         Property(&DataSlice::GetShape,
                                  IsEquivalentTo(expected_keys_shape)))));
}

TEST(ObjectFactoriesTest, CreateDictShaped_WithValues_WithSchema) {
  auto shape = DataSlice::JaggedShape::FlatFromSize(3);
  auto db = DataBag::Empty();
  ASSERT_OK_AND_ASSIGN(
      auto ds,
      CreateDictShaped(db, shape, /*keys=*/test::DataSlice<int32_t>({1, 2, 3}),
                       /*values=*/test::DataSlice<int32_t>({57, 58, 59}),
                       /*key_schema=*/test::Schema(schema::kInt64),
                       /*value_schema=*/test::Schema(schema::kInt64)));
  EXPECT_THAT(
      ds.slice(),
      ElementsAre(
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue())),
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue())),
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue()))));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(shape));
  EXPECT_THAT(ds.GetSchema().GetAttr("__keys__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kInt64)));
  EXPECT_THAT(ds.GetSchema().GetAttr("__values__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kInt64)));
  ASSERT_OK_AND_ASSIGN(
      auto expected_keys_shape,
      shape->AddDims({DenseArrayEdge::FromUniformGroups(3, 1).value()}));
  EXPECT_THAT(
      ds.GetDictKeys(),
      IsOkAndHolds(AllOf(Property(&DataSlice::slice,
                                  ElementsAre(DataItemWith<int64_t>(1), 2, 3)),
                         Property(&DataSlice::GetShape,
                                  IsEquivalentTo(expected_keys_shape)))));
}

TEST(ObjectFactoriesTest, CreateDictShaped_Errors) {
  auto db = DataBag::Empty();
  auto shape = DataSlice::JaggedShape::FlatFromSize(3);

  EXPECT_THAT(
      CreateDictShaped(db, shape,
                       /*keys=*/test::DataSlice<int32_t>({1, 2}),
                       /*values=*/std::nullopt,
                       /*key_schema=*/std::nullopt,
                       /*value_schema=*/std::nullopt),
      StatusIs(absl::StatusCode::kInvalidArgument,
               "creating a dict requires both keys and values, got only keys"));
  EXPECT_THAT(
      CreateDictShaped(db, shape, /*keys=*/std::nullopt,
                       /*values=*/test::DataSlice<int32_t>({1, 2}),
                       /*key_schema=*/std::nullopt,
                       /*value_schema=*/std::nullopt),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          "creating a dict requires both keys and values, got only values"));
  EXPECT_THAT(CreateDictShaped(db, shape,
                               /*keys=*/std::nullopt,
                               /*values=*/std::nullopt,
                               /*key_schema=*/test::Schema(schema::kFloat32),
                               /*value_schema=*/test::Schema(schema::kInt32)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "dict keys cannot be FLOAT32"));
}

TEST(ObjectFactoriesTest, CreateDictLike) {
  ASSERT_OK_AND_ASSIGN(auto edge1, DenseArrayEdge::FromSplitPoints(
                                       CreateDenseArray<int64_t>({0, 2})));
  ASSERT_OK_AND_ASSIGN(auto edge2, DenseArrayEdge::FromSplitPoints(
                                       CreateDenseArray<int64_t>({0, 2, 4})));
  ASSERT_OK_AND_ASSIGN(auto shape, DataSlice::JaggedShape::FromEdges(
                                       {std::move(edge1), std::move(edge2)}));
  auto db = DataBag::Empty();
  auto shape_and_mask = test::MixedDataSlice<int, Text>(
      {1, std::nullopt, std::nullopt, 3},
      {std::nullopt, "foo", std::nullopt, std::nullopt}, shape);

  ASSERT_OK_AND_ASSIGN(
      auto ds,
      CreateDictLike(db, shape_and_mask, /*keys=*/std::nullopt,
                     /*values=*/std::nullopt, /*key_schema=*/std::nullopt,
                     /*value_schema=*/std::nullopt));
  EXPECT_THAT(
      ds.slice(),
      ElementsAre(
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue())),
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue())),
          MissingDataItem(),
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue()))));
  EXPECT_THAT(
      ds.slice().allocation_ids(),
      ElementsAre(Property(&internal::AllocationId::IsDictsAlloc, IsTrue())));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(shape));
  EXPECT_THAT(ds.GetSchema().GetAttr("__keys__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kObject)));
  EXPECT_THAT(ds.GetSchema().GetAttr("__values__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kObject)));
}

TEST(ObjectFactoriesTest, CreateDictLike_WithValues) {
  ASSERT_OK_AND_ASSIGN(auto edge1, DenseArrayEdge::FromSplitPoints(
                                       CreateDenseArray<int64_t>({0, 2})));
  ASSERT_OK_AND_ASSIGN(auto edge2, DenseArrayEdge::FromSplitPoints(
                                       CreateDenseArray<int64_t>({0, 2, 4})));
  ASSERT_OK_AND_ASSIGN(auto shape, DataSlice::JaggedShape::FromEdges(
                                       {std::move(edge1), std::move(edge2)}));
  auto db = DataBag::Empty();
  auto shape_and_mask = test::MixedDataSlice<int, Text>(
      {1, std::nullopt, std::nullopt, 3},
      {std::nullopt, "foo", std::nullopt, std::nullopt}, shape);

  ASSERT_OK_AND_ASSIGN(
      auto ds, CreateDictLike(db, shape_and_mask,
                              /*keys=*/test::DataSlice<int32_t>({1, 2}),
                              /*values=*/test::DataSlice<int32_t>({57, 58}),
                              /*key_schema=*/std::nullopt,
                              /*value_schema=*/std::nullopt));
  EXPECT_THAT(
      ds.slice(),
      ElementsAre(
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue())),
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue())),
          MissingDataItem(),
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue()))));
  EXPECT_THAT(
      ds.slice().allocation_ids(),
      ElementsAre(Property(&internal::AllocationId::IsDictsAlloc, IsTrue())));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(shape));
  EXPECT_THAT(ds.GetSchema().GetAttr("__keys__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kInt32)));
  EXPECT_THAT(ds.GetSchema().GetAttr("__values__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kInt32)));
  ASSERT_OK_AND_ASSIGN(
      auto expected_keys_shape,
      shape->AddDims({DenseArrayEdge::FromSplitPoints(
                          CreateDenseArray<int64_t>({0, 1, 2, 2, 3}))
                          .value()}));
  EXPECT_THAT(
      ds.GetDictKeys(),
      IsOkAndHolds(AllOf(Property(&DataSlice::slice,
                                  ElementsAre(DataItemWith<int32_t>(1), 1, 2)),
                         Property(&DataSlice::GetShape,
                                  IsEquivalentTo(expected_keys_shape)))));
}

TEST(ObjectFactoriesTest, CreateDictLike_WithValues_WithSchema) {
  ASSERT_OK_AND_ASSIGN(auto edge1, DenseArrayEdge::FromSplitPoints(
                                       CreateDenseArray<int64_t>({0, 2})));
  ASSERT_OK_AND_ASSIGN(auto edge2, DenseArrayEdge::FromSplitPoints(
                                       CreateDenseArray<int64_t>({0, 2, 4})));
  ASSERT_OK_AND_ASSIGN(auto shape, DataSlice::JaggedShape::FromEdges(
                                       {std::move(edge1), std::move(edge2)}));
  auto db = DataBag::Empty();
  auto shape_and_mask = test::MixedDataSlice<int, Text>(
      {1, std::nullopt, std::nullopt, 3},
      {std::nullopt, "foo", std::nullopt, std::nullopt}, shape);

  ASSERT_OK_AND_ASSIGN(
      auto ds, CreateDictLike(db, shape_and_mask,
                              /*keys=*/test::DataSlice<int32_t>({1, 2}),
                              /*values=*/test::DataSlice<int32_t>({57, 58}),
                              /*key_schema=*/test::Schema(schema::kInt64),
                              /*value_schema=*/test::Schema(schema::kInt64)));
  EXPECT_THAT(
      ds.slice(),
      ElementsAre(
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue())),
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue())),
          MissingDataItem(),
          DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue()))));
  EXPECT_THAT(
      ds.slice().allocation_ids(),
      ElementsAre(Property(&internal::AllocationId::IsDictsAlloc, IsTrue())));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(shape));
  EXPECT_THAT(ds.GetSchema().GetAttr("__keys__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kInt64)));
  EXPECT_THAT(ds.GetSchema().GetAttr("__values__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kInt64)));
  ASSERT_OK_AND_ASSIGN(
      auto expected_keys_shape,
      shape->AddDims({DenseArrayEdge::FromSplitPoints(
                          CreateDenseArray<int64_t>({0, 1, 2, 2, 3}))
                          .value()}));
  EXPECT_THAT(
      ds.GetDictKeys(),
      IsOkAndHolds(AllOf(Property(&DataSlice::slice,
                                  ElementsAre(DataItemWith<int64_t>(1), 1, 2)),
                         Property(&DataSlice::GetShape,
                                  IsEquivalentTo(expected_keys_shape)))));
}

TEST(ObjectFactoriesTest, CreateDictLike_DataItem) {
  auto db = DataBag::Empty();
  auto shape_and_mask = test::DataItem(57, schema::kAny, db);

  ASSERT_OK_AND_ASSIGN(
      auto ds,
      CreateDictLike(db, shape_and_mask, /*keys=*/std::nullopt,
                     /*values=*/std::nullopt, /*key_schema=*/std::nullopt,
                     /*value_schema=*/std::nullopt));
  EXPECT_THAT(ds.item(),
              DataItemWith<ObjectId>(Property(&ObjectId::IsDict, IsTrue())));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(*DataSlice::JaggedShape::Empty()));
  EXPECT_THAT(ds.GetSchema().GetAttr("__keys__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kObject)));
  EXPECT_THAT(ds.GetSchema().GetAttr("__values__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kObject)));
}

TEST(ObjectFactoriesTest, CreateDictLike_MissingDataItem) {
  auto db = DataBag::Empty();
  auto shape_and_mask = test::DataItem(internal::DataItem(), schema::kAny, db);

  ASSERT_OK_AND_ASSIGN(
      auto ds,
      CreateDictLike(db, shape_and_mask, /*keys=*/std::nullopt,
                     /*values=*/std::nullopt, /*key_schema=*/std::nullopt,
                     /*value_schema=*/std::nullopt));
  EXPECT_THAT(ds.item(), MissingDataItem());
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(shape_and_mask.GetShape()));
  EXPECT_THAT(ds.GetSchema().GetAttr("__keys__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kObject)));
  EXPECT_THAT(ds.GetSchema().GetAttr("__values__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kObject)));
}

TEST(ObjectFactoriesTest, CreateDictLike_Errors) {
  auto db = DataBag::Empty();
  auto shape_and_mask = test::DataItem(internal::DataItem(), schema::kAny, db);

  EXPECT_THAT(
      CreateDictLike(db, shape_and_mask,
                     /*keys=*/test::DataSlice<int32_t>({1, 2}),
                     /*values=*/std::nullopt,
                     /*key_schema=*/std::nullopt,
                     /*value_schema=*/std::nullopt),
      StatusIs(absl::StatusCode::kInvalidArgument,
               "creating a dict requires both keys and values, got only keys"));
  EXPECT_THAT(
      CreateDictLike(db, shape_and_mask, /*keys=*/std::nullopt,
                     /*values=*/test::DataSlice<int32_t>({1, 2}),
                     /*key_schema=*/std::nullopt,
                     /*value_schema=*/std::nullopt),
      StatusIs(
          absl::StatusCode::kInvalidArgument,
          "creating a dict requires both keys and values, got only values"));
  EXPECT_THAT(CreateDictLike(db, shape_and_mask,
                             /*keys=*/std::nullopt,
                             /*values=*/std::nullopt,
                             /*key_schema=*/test::Schema(schema::kFloat32),
                             /*value_schema=*/test::Schema(schema::kInt32)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       "dict keys cannot be FLOAT32"));
}

TEST(ObjectFactoriesTest, CreateListLike) {
  ASSERT_OK_AND_ASSIGN(auto edge1, DenseArrayEdge::FromSplitPoints(
                                       CreateDenseArray<int64_t>({0, 2})));
  ASSERT_OK_AND_ASSIGN(auto edge2, DenseArrayEdge::FromSplitPoints(
                                       CreateDenseArray<int64_t>({0, 2, 4})));
  ASSERT_OK_AND_ASSIGN(auto shape, DataSlice::JaggedShape::FromEdges(
                                       {std::move(edge1), std::move(edge2)}));
  auto db = DataBag::Empty();
  auto shape_and_mask = test::MixedDataSlice<int, Text>(
      {1, std::nullopt, std::nullopt, 3},
      {std::nullopt, "foo", std::nullopt, std::nullopt}, shape);

  ASSERT_OK_AND_ASSIGN(auto ds, CreateListLike(db, shape_and_mask,
                                               /*values=*/std::nullopt,
                                               test::Schema(schema::kInt32)));
  EXPECT_THAT(
      ds.slice(),
      ElementsAre(
          DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue())),
          DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue())),
          MissingDataItem(),
          DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue()))));
  EXPECT_THAT(
      ds.slice().allocation_ids(),
      ElementsAre(Property(&internal::AllocationId::IsListsAlloc, IsTrue())));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(shape));
  EXPECT_THAT(ds.GetSchema().GetAttr("__items__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kInt32)));
}

TEST(ObjectFactoriesTest, CreateListLike_WithValues) {
  ASSERT_OK_AND_ASSIGN(auto edge1, DenseArrayEdge::FromSplitPoints(
                                       CreateDenseArray<int64_t>({0, 2})));
  ASSERT_OK_AND_ASSIGN(auto edge2, DenseArrayEdge::FromSplitPoints(
                                       CreateDenseArray<int64_t>({0, 2, 4})));
  ASSERT_OK_AND_ASSIGN(auto shape, DataSlice::JaggedShape::FromEdges(
                                       {std::move(edge1), std::move(edge2)}));
  auto db = DataBag::Empty();
  auto shape_and_mask = test::MixedDataSlice<int, Text>(
      {1, std::nullopt, std::nullopt, 3},
      {std::nullopt, "foo", std::nullopt, std::nullopt}, shape);

  ASSERT_OK_AND_ASSIGN(
      auto ds, CreateListLike(db, shape_and_mask,
                              /*values=*/test::DataSlice<int32_t>({1, 2}),
                              /*item_schema=*/std::nullopt));
  EXPECT_THAT(
      ds.slice(),
      ElementsAre(
          DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue())),
          DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue())),
          MissingDataItem(),
          DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue()))));
  EXPECT_THAT(
      ds.slice().allocation_ids(),
      ElementsAre(Property(&internal::AllocationId::IsListsAlloc, IsTrue())));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(shape));
  EXPECT_THAT(ds.GetSchema().GetAttr("__items__"),
              // Deduced from values.
              IsOkAndHolds(Property(&DataSlice::item, schema::kInt32)));
  EXPECT_THAT(ds.ExplodeList(0, std::nullopt),
              IsOkAndHolds(Property(&DataSlice::slice, ElementsAre(1, 1, 2))));
}

TEST(ObjectFactoriesTest, CreateListLike_DataItem) {
  auto db = DataBag::Empty();
  auto shape_and_mask = test::DataItem(57);

  ASSERT_OK_AND_ASSIGN(
      auto ds, CreateListLike(db, shape_and_mask,
                              /*values=*/
                              std::nullopt, test::Schema(schema::kInt32)));
  EXPECT_THAT(ds.item(),
              DataItemWith<ObjectId>(Property(&ObjectId::IsList, IsTrue())));
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(*DataSlice::JaggedShape::Empty()));
  EXPECT_THAT(ds.GetSchema().GetAttr("__items__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kInt32)));
}

TEST(ObjectFactoriesTest, CreateListLike_MissingDataItem) {
  ASSERT_OK_AND_ASSIGN(auto shape, DataSlice::JaggedShape::FromEdges({}));
  auto db = DataBag::Empty();
  auto shape_and_mask = test::DataItem(internal::DataItem());

  ASSERT_OK_AND_ASSIGN(auto ds, CreateListLike(db, shape_and_mask,
                                               /*values=*/std::nullopt,
                                               test::Schema(schema::kInt32)));
  EXPECT_THAT(ds.item(), MissingDataItem());
  EXPECT_EQ(ds.GetDb(), db);
  EXPECT_THAT(ds.GetShape(), IsEquivalentTo(shape));
  EXPECT_THAT(ds.GetSchema().GetAttr("__items__"),
              IsOkAndHolds(Property(&DataSlice::item, schema::kInt32)));
}

TEST(ObjectFactoriesTest, CreateUuidFromFields_DataSlice) {
  constexpr int64_t kSize = 3;
  auto shape = DataSlice::JaggedShape::FlatFromSize(kSize);

  auto ds_a = test::AllocateDataSlice(3, schema::kObject);
  auto ds_b = test::AllocateDataSlice(3, schema::kObject);

  ASSERT_OK_AND_ASSIGN(
      auto ds,
      CreateUuidFromFields("", {std::string("a"), std::string("b")},
                           {ds_a, ds_b}));
  // Schema check.
  EXPECT_EQ(ds.GetSchemaImpl(), schema::kItemId);

  // DataSlice checks.
  EXPECT_EQ(ds.size(), kSize);
  EXPECT_EQ(ds.dtype(), arolla::GetQType<ObjectId>());

  ASSERT_OK_AND_ASSIGN(auto expected,
                       internal::CreateUuidFromFields(
                           "", {{std::string("a"), ds_a.slice()},
                                {std::string("b"), ds_b.slice()}}));
  EXPECT_THAT(ds.slice().values<ObjectId>(), ElementsAreArray(
      expected.values<ObjectId>()));

  // Seeded UUIds
  ASSERT_OK_AND_ASSIGN(
      auto ds_with_seed_1,
      CreateUuidFromFields("seed_1", {std::string("a"), std::string("b")},
                           {ds_a, ds_b}));
  ASSERT_OK_AND_ASSIGN(
      auto ds_with_seed_2,
      CreateUuidFromFields("seed_2", {std::string("a"), std::string("b")},
                           {ds_a, ds_b}));
  ASSERT_OK_AND_ASSIGN(expected,
                       internal::CreateUuidFromFields(
                           "seed_1",    {{std::string("a"), ds_a.slice()},
                                         {std::string("b"), ds_b.slice()}}));
  EXPECT_THAT(ds_with_seed_1.slice().values<ObjectId>(), ElementsAreArray(
      expected.values<ObjectId>()));
  EXPECT_THAT(ds_with_seed_1.slice().values<ObjectId>(),
              Not(ElementsAreArray(
                  ds_with_seed_2.slice().values<ObjectId>())));
}

TEST(ObjectFactoriesTest, CreateUuidFromFields_DataItem) {
  auto ds_a = test::DataItem(internal::AllocateSingleObject());
  auto ds_b = test::DataItem(42);

  ASSERT_OK_AND_ASSIGN(
      auto ds,
      CreateUuidFromFields("",
                           {std::string("a"), std::string("b")}, {ds_a, ds_b}));

  // Schema check.
  EXPECT_EQ(ds.GetSchemaImpl(), schema::kItemId);

  // DataItem checks.
  EXPECT_EQ(ds.size(), 1);
  EXPECT_EQ(ds.GetShape().rank(), 0);
  EXPECT_EQ(ds.dtype(), arolla::GetQType<ObjectId>());
  auto expected = internal::CreateUuidFromFields("",
                           {{std::string("a"), ds_a.item()},
                            {std::string("b"), ds_b.item()}});
  EXPECT_EQ(ds.item(), expected);

  ASSERT_OK_AND_ASSIGN(
      auto ds_with_seed_1,
      CreateUuidFromFields("seed_1", {std::string("a"), std::string("b")},
                           {ds_a, ds_b}));
  ASSERT_OK_AND_ASSIGN(
      auto ds_with_seed_2,
      CreateUuidFromFields("seed_2", {std::string("a"), std::string("b")},
                           {ds_a, ds_b}));

  expected = internal::CreateUuidFromFields(
      "seed_1",
      {{std::string("a"), ds_a.item()}, {std::string("b"), ds_b.item()}});
  EXPECT_EQ(ds_with_seed_1.item(), expected);
  EXPECT_NE(ds_with_seed_1.item(), ds_with_seed_2.item());
}

TEST(ObjectFactoriesTest, CreateUuidFromFields_Empty) {
  ASSERT_OK_AND_ASSIGN(auto ds, CreateUuidFromFields("", {}, {}));
  EXPECT_EQ(ds.size(), 1);

  EXPECT_EQ(ds.GetShape().rank(), 0);
  EXPECT_EQ(ds.dtype(), arolla::GetQType<ObjectId>());
  EXPECT_EQ(ds.GetSchemaImpl(), schema::kItemId);

  EXPECT_TRUE(ds.item().value<ObjectId>().IsUuid());
  EXPECT_FALSE(ds.item().value<ObjectId>().IsSchema());
}

TEST(ObjectFactoriesTest, CreateNoFollowSchema_EntitySchema) {
  auto db = DataBag::Empty();
  auto ds_primitives = test::DataSlice<int>({1, 2, 3});
  ASSERT_OK_AND_ASSIGN(auto ds, EntityCreator()(db, {"a"}, {ds_primitives}));
  ASSERT_OK_AND_ASSIGN(auto nofollow_schema,
                       CreateNoFollowSchema(ds.GetSchema()));
  ASSERT_TRUE(nofollow_schema.item().holds_value<internal::ObjectId>());
  EXPECT_TRUE(
      nofollow_schema.item().value<internal::ObjectId>().IsNoFollowSchema());
  EXPECT_THAT(nofollow_schema.GetNoFollowedSchema(),
              IsOkAndHolds(Property(&DataSlice::item, ds.GetSchemaImpl())));
}

TEST(ObjectFactoriesTest, CreateNoFollowSchema_ObjectSchema) {
  auto db = DataBag::Empty();
  auto ds_primitives = test::DataSlice<int>({1, 2, 3});
  ASSERT_OK_AND_ASSIGN(auto ds, ObjectCreator()(db, {"a"}, {ds_primitives}));
  ASSERT_OK_AND_ASSIGN(auto nofollow_schema,
                       CreateNoFollowSchema(ds.GetSchema()));
  ASSERT_TRUE(nofollow_schema.item().holds_value<internal::ObjectId>());
  EXPECT_TRUE(
      nofollow_schema.item().value<internal::ObjectId>().IsNoFollowSchema());
  EXPECT_THAT(nofollow_schema.GetNoFollowedSchema(),
              IsOkAndHolds(Property(&DataSlice::item, schema::kObject)));
}

TEST(ObjectFactoriesTest, CreateNoFollowSchema_PrimitiveSchema) {
  auto db = DataBag::Empty();
  EXPECT_THAT(
      CreateNoFollowSchema(test::Schema(schema::kInt32)),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("calling nofollow on INT32 slice is not allowed")));
}

TEST(ObjectFactoriesTest, CreateNoFollowSchema_AnySchema) {
  auto db = DataBag::Empty();
  EXPECT_THAT(
      CreateNoFollowSchema(test::Schema(schema::kAny)),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("calling nofollow on ANY slice is not allowed")));
}

TEST(ObjectFactoriesTest, CreateNoFollowSchema_ItemIdSchema) {
  auto db = DataBag::Empty();
  EXPECT_THAT(
      CreateNoFollowSchema(test::Schema(schema::kItemId)),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("calling nofollow on ITEMID slice is not allowed")));
}

TEST(ObjectFactoriesTest, CreateNoFollowSchema_NonSchema) {
  auto db = DataBag::Empty();
  EXPECT_THAT(CreateNoFollowSchema(test::DataItem(42)),
              StatusIs(absl::StatusCode::kInvalidArgument,
                       HasSubstr("schema's schema must be SCHEMA")));
}

TEST(ObjectFactoriesTest, NoFollow_Entity) {
  auto db = DataBag::Empty();
  auto ds_primitives = test::DataSlice<int>({1, 2, 3});
  ASSERT_OK_AND_ASSIGN(auto ds, EntityCreator()(db, {"a"}, {ds_primitives}));
  ASSERT_OK_AND_ASSIGN(auto nofollow, NoFollow(ds));
  EXPECT_THAT(nofollow.slice(), IsEquivalentTo(ds.slice()));
  EXPECT_TRUE(
      nofollow.GetSchemaImpl().value<internal::ObjectId>().IsNoFollowSchema());
  EXPECT_THAT(nofollow.GetSchema().GetNoFollowedSchema(),
              IsOkAndHolds(Property(&DataSlice::item, ds.GetSchemaImpl())));
}

TEST(ObjectFactoriesTest, NoFollow_Objects) {
  auto db = DataBag::Empty();
  auto ds_primitives = test::DataSlice<int>({1, 2, 3});
  ASSERT_OK_AND_ASSIGN(auto ds, ObjectCreator()(db, {"a"}, {ds_primitives}));
  ASSERT_OK_AND_ASSIGN(auto nofollow, NoFollow(ds));
  EXPECT_THAT(nofollow.slice(), IsEquivalentTo(ds.slice()));
  EXPECT_TRUE(
      nofollow.GetSchemaImpl().value<internal::ObjectId>().IsNoFollowSchema());
  EXPECT_THAT(nofollow.GetSchema().GetNoFollowedSchema(),
              IsOkAndHolds(Property(&DataSlice::item, schema::kObject)));
}

TEST(ObjectFactoriesTest, NoFollow_On_NoFollow) {
  auto db = DataBag::Empty();
  auto ds_primitives = test::DataSlice<int>({1, 2, 3});
  ASSERT_OK_AND_ASSIGN(auto ds, EntityCreator()(db, {"a"}, {ds_primitives}));
  ASSERT_OK_AND_ASSIGN(auto nofollow, NoFollow(ds));
  EXPECT_THAT(
      NoFollow(nofollow),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("nofollow on a nofollow slice is not allowed")));
}

TEST(ObjectFactoriesTest, NoFollow_Primitives) {
  auto db = DataBag::Empty();
  auto ds = test::DataSlice<int>({1, 2, 3});
  EXPECT_THAT(
      NoFollow(ds),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("calling nofollow on INT32 slice is not allowed")));
}

TEST(ObjectFactoriesTest, NoFollow_Any) {
  auto db = DataBag::Empty();
  auto ds_primitives = test::DataSlice<int>({1, 2, 3});
  ASSERT_OK_AND_ASSIGN(auto ds, ObjectCreator()(db, {"a"}, {ds_primitives}));
  ASSERT_OK_AND_ASSIGN(ds, ds.WithSchema(test::Schema(schema::kAny)));
  EXPECT_THAT(
      NoFollow(ds),
      StatusIs(absl::StatusCode::kInvalidArgument,
               HasSubstr("calling nofollow on ANY slice is not allowed")));
}

}  // namespace
}  // namespace koladata
