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
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"
#include "gmock/gmock.h"
#include "absl/log/check.h"
#include "absl/random/distributions.h"
#include "absl/random/random.h"
#include "absl/strings/str_cat.h"
#include "koladata/internal/data_bag.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/data_slice.h"
#include "koladata/internal/dtype.h"
#include "koladata/internal/object_id.h"
#include "koladata/internal/op_utils/extract.h"
#include "koladata/internal/op_utils/presence_and.h"
#include "arolla/dense_array/dense_array.h"
#include "arolla/memory/optional_value.h"
#include "arolla/qtype/base_types.h"
#include "arolla/util/unit.h"

namespace koladata::internal {
namespace {

constexpr auto kBenchmarkFn = [](auto* b) {
  // Number of objects per chain, number of chains.
  b->Args({1, 1})
      ->Args({1, 1000})
      ->Args({1, 1'000'000})
      ->Args({2, 1})
      ->Args({2, 1000})
      ->Args({2, 1'000'000})
      ->Args({1000, 1})
      ->Args({1000, 1000})
      ->Args({1'000'000, 1});
};

constexpr auto kLayersBenchmarkFn = [](auto* b) {
  // Number of layers, number of attributes per object, percent of present
  // attributes, number of objects per layer.
  b->Args({2, 10, 100, 1000})
      ->Args({2, 100, 100, 1000})
      ->Args({2, 100, 100, 10})
      ->Args({20, 10, 100, 10})
      ->Args({20, 100, 100, 10})
      ->Args({20, 100, 10, 10});
};

void RunBenchmarks(benchmark::State& state, DataSliceImpl& ds, DataItem& schema,
                   DataBagImplPtr& databag,
                   DataBagImpl::FallbackSpan fallbacks = {}) {
  while (state.KeepRunning()) {
    benchmark::DoNotOptimize(ds);
    benchmark::DoNotOptimize(schema);
    benchmark::DoNotOptimize(databag);
    benchmark::DoNotOptimize(fallbacks);
    auto result_db = DataBagImpl::CreateEmptyDatabag();
    ExtractOp(result_db.get())(ds, schema, *databag, fallbacks).IgnoreError();
    benchmark::DoNotOptimize(result_db);
  }
}

DataSliceImpl AllocateSliceOfObjects(int64_t size) {
  auto obj_alloc = internal::Allocate(size);
  DataSliceImpl::Builder bldr(size);
  for (int64_t i = 0; i < size; ++i) {
    bldr.Insert(i, obj_alloc.ObjectByOffset(i));
  }
  return std::move(bldr).Build();
}

DataSliceImpl ShuffleObjectsSlice(const DataSliceImpl& ds, absl::BitGen& gen) {
  // Generate random permutation.
  std::vector<int64_t> perm(ds.size());
  for (int64_t i = 0; i < ds.size(); ++i) {
    perm[i] = i;
  }
  for (int64_t i = 1; i < ds.size(); ++i) {
    std::swap(perm[i], perm[absl::Uniform(gen, 0, i)]);
  }
  // Shuffle data.
  auto obj_ids_bldr = arolla::DenseArrayBuilder<ObjectId>(ds.size());
  const auto& values = ds.values<ObjectId>();
  for (int64_t i = 0; i < ds.size(); ++i) {
    obj_ids_bldr.Set(perm[i], values[i]);
  }
  return DataSliceImpl::CreateObjectsDataSlice(std::move(obj_ids_bldr).Build(),
                                               ds.allocation_ids());
}

DataSliceImpl ApplyRandomMask(const DataSliceImpl& ds, int64_t presence_rate,
                              absl::BitGen& gen) {
  auto filter = arolla::DenseArrayBuilder<arolla::Unit>(ds.size());
  for (int64_t i = 0; i < ds.size(); ++i) {
    if (absl::Uniform(gen, 0, 100) < presence_rate) {
      filter.Set(i, arolla::kPresent);
    }
  }
  auto ds_filter = DataSliceImpl::Create(std::move(filter).Build());
  auto result = PresenceAndOp()(ds, ds_filter).value();
  return result;
}

void BM_DisjointChains(benchmark::State& state) {
  int64_t schema_depth = state.range(0);
  int64_t ds_size = state.range(1);

  auto db = DataBagImpl::CreateEmptyDatabag();
  auto root_ds = AllocateSliceOfObjects(ds_size);
  auto root_schema = DataItem(internal::AllocateExplicitSchema());
  auto ds = root_ds;
  auto schema = root_schema;
  const internal::DataItem kObjectSchema(schema::kObject);
  for (int64_t i = 0; i < schema_depth; ++i) {
    auto child_schema = DataItem(internal::AllocateExplicitSchema());
    auto child_ds = AllocateSliceOfObjects(ds_size);
    EXPECT_OK(db->SetSchemaAttr(schema, "child", child_schema));
    EXPECT_OK(db->SetAttr(ds, "child", child_ds));
    ds = child_ds;
    schema = child_schema;
  }
  RunBenchmarks(state, root_ds, root_schema, db);
}

BENCHMARK(BM_DisjointChains)->Apply(kBenchmarkFn);

void BM_DAG(benchmark::State& state) {
  int64_t schema_depth = state.range(0);
  int64_t attr_count = state.range(1);
  int64_t presence_rate = state.range(2);
  int64_t ds_size = state.range(3);
  absl::BitGen gen;

  auto db = DataBagImpl::CreateEmptyDatabag();
  auto root_ds = AllocateSliceOfObjects(ds_size);
  auto root_schema = DataItem(internal::AllocateExplicitSchema());
  auto ds = root_ds;
  auto schema = root_schema;
  for (int64_t i = 0; i < schema_depth; ++i) {
    auto child_schema = DataItem(internal::AllocateExplicitSchema());
    auto child_ds = AllocateSliceOfObjects(ds_size);
    for (int64_t j = 0; j < attr_count; ++j) {
      std::string attr_name = absl::StrCat("layer_", i, "_child_", j);
      EXPECT_OK(db->SetSchemaAttr(schema, attr_name, child_schema));
      EXPECT_OK(db->SetAttr(ds, attr_name,
                            ApplyRandomMask(ShuffleObjectsSlice(child_ds, gen),
                                            presence_rate, gen)));
    }
    ds = child_ds;
    schema = child_schema;
  }
  RunBenchmarks(state, root_ds, root_schema, db);
}

BENCHMARK(BM_DAG)->Apply(kLayersBenchmarkFn);

}  // namespace
}  // namespace koladata::internal
