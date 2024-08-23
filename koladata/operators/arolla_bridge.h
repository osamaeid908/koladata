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
#ifndef KOLADATA_OPERATORS_AROLLA_BRIDGE_H_
#define KOLADATA_OPERATORS_AROLLA_BRIDGE_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "absl/hash/hash.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "koladata/data_slice.h"
#include "koladata/data_slice_qtype.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/data_slice.h"
#include "koladata/internal/dtype.h"
#include "arolla/dense_array/dense_array.h"
#include "arolla/dense_array/qtype/types.h"
#include "arolla/memory/optional_value.h"
#include "arolla/qtype/qtype.h"
#include "arolla/qtype/typed_ref.h"
#include "arolla/qtype/typed_value.h"
#include "arolla/util/text.h"
#include "arolla/util/unit.h"

namespace koladata::ops {
namespace compiler_internal {

using CompiledOp = std::function<absl::StatusOr<::arolla::TypedValue>(
    absl::Span<const arolla::TypedRef>)>;

// Key for the compilation cache consisting of:
// * op_name: the name of the operator.
// * input_qtypes: the QTypes of the inputs.
//
// Exposed for testing purposes.
struct Key {
  std::string op_name;
  std::vector<arolla::QTypePtr> input_qtypes;

  template <typename H>
  friend H AbslHashValue(H h, const Key& key) {
    // NOTE: Must be compatible with `LookupKey` below.
    h = H::combine(std::move(h), key.op_name, key.input_qtypes.size());
    for (const auto& input_qtype : key.input_qtypes) {
      h = H::combine(std::move(h), input_qtype);
    }
    return h;
  }
};

// Lookup-key for the compilation cache consisting of:
// * op_name: the name of the operator.
// * input_qvalues: the TypedRef inputs.
//
// NOTE: Must be compatible with `Key` above. The type of each
// input, together with the op_name, is used to compared with the stored keys.
//
// Exposed for testing purposes.
struct LookupKey {
  absl::string_view op_name;
  absl::Span<const arolla::TypedRef> input_qvalues;

  template <typename H>
  friend H AbslHashValue(H h, const LookupKey& lookup_key) {
    // NOTE: Must be compatible with `Key` above.
    h = H::combine(std::move(h), lookup_key.op_name,
                   lookup_key.input_qvalues.size());
    for (const auto& input_qvalue : lookup_key.input_qvalues) {
      h = H::combine(std::move(h), input_qvalue.GetType());
    }
    return h;
  }
};

// Hashing functor for the compilation cache. Supports both
// `Key` and `LookupKey`.
//
// Exposed for testing purposes.
struct KeyHash {
  using is_transparent = void;

  size_t operator()(const Key& key) const { return absl::HashOf(key); }
  size_t operator()(const LookupKey& key) const { return absl::HashOf(key); }
};

// Equality functor for the compilation cache. Supports both
// `Key` and `LookupKey`.
struct KeyEq {
  using is_transparent = void;

  bool operator()(const Key& lhs, const Key& rhs) const {
    return lhs.op_name == rhs.op_name && lhs.input_qtypes == rhs.input_qtypes;
  }
  bool operator()(const Key& lhs, const LookupKey& rhs) const {
    return lhs.op_name == rhs.op_name &&
           std::equal(lhs.input_qtypes.begin(), lhs.input_qtypes.end(),
                      rhs.input_qvalues.begin(), rhs.input_qvalues.end(),
                      [](const arolla::QTypePtr& ltype,
                         const arolla::TypedRef& rvalue) {
                        return ltype == rvalue.GetType();
                      });
  }
  bool operator()(const LookupKey& lhs, const Key& rhs) const {
    return this->operator()(rhs, lhs);
  }
  bool operator()(const LookupKey& lhs, const LookupKey& rhs) const {
    return lhs.op_name == rhs.op_name &&
           std::equal(lhs.input_qvalues.begin(), lhs.input_qvalues.end(),
                      rhs.input_qvalues.begin(), rhs.input_qvalues.end(),
                      [](const arolla::TypedRef& lvalue,
                         const arolla::TypedRef& rvalue) {
                        return lvalue.GetType() == rvalue.GetType();
                      });
  }
};

// Returns the compilation cache entry for the given op_name and inputs. May be
// null if the entry is not found.
//
// Exposed for testing purposes.
CompiledOp Lookup(absl::string_view op_name,
                  absl::Span<const arolla::TypedRef> inputs);

// Clears the compilation cache.
//
// Exposed for testing purposes.
void ClearCache();

}  // namespace compiler_internal

// Evaluates the registered operator of the given name on the given inputs,
// using a compilation cache.
absl::StatusOr<arolla::TypedValue> EvalExpr(
    absl::string_view op_name, absl::Span<const arolla::TypedRef> inputs);

// Returns the schema of the data of `x` that is compatible with Arolla.
// * If the schema of `x` is a primitive schema, returns it.
// * If the schema is given by the data (e.g. in the case of `OBJECT`), the
//   type of the data is returned as a primitive schema if possible.
// * If the slice is empty and the type cannot be inferred from its schema,
//   an empty internal::DataItem is returned.
// * Otherwise, an error is returned.
absl::StatusOr<internal::DataItem> GetPrimitiveArollaSchema(const DataSlice& x);

// Evaluates the registered operator of the given name on the given inputs and
// returns the result. The expr_op is expected to be a pointwise operator that
// should be evaluated on the given inputs extracted as Arolla values. The
// output DataSlice's shape is the common shape of the inputs. Its schema is
// either the common schema of the inputs' schemas and schema derived from
// Arolla output, or `output_schema` if provided. If all inputs are
// empty-and-unknown, the `expr_op` is not evaluated. In other cases the first
// primitive schema of the present inputs is used to construct inputs.
absl::StatusOr<DataSlice> SimplePointwiseEval(
    absl::string_view op_name, std::vector<DataSlice> inputs,
    internal::DataItem output_schema = internal::DataItem());

// Evaluates the registered operator of the given name on the given input and
// returns the result. The expr_op is expected to be an agg-into operator that
// should be evaluated on `x` extracted as an Arolla value and the edge of the
// last dimension. The output DataSlice has the shape of `x` with the last
// dimension removed and the schema of `x`, or `output_schema` if provided. If
// `x` is empty-and-unknown, the `expr_op` is not evaluated. The
// `edge_arg_index` specifies the index of the argument where to insert the edge
// when passed to the `expr_op`.
absl::StatusOr<DataSlice> SimpleAggIntoEval(
    absl::string_view op_name, std::vector<DataSlice> inputs,
    internal::DataItem output_schema = internal::DataItem(),
    int edge_arg_index = 1);

// Evaluates the registered operator of the given name on the given input and
// returns the result. The expr_op is expected to be an agg-over operator that
// should be evaluated on `x` extracted as an Arolla value and the edge of the
// last dimension. The output DataSlice has the the shape of `x` and the schema
// of `x`, or `output_schema` if provided. If `x` is empty-and-unknown, the
// `expr_op` is not evaluated. The `edge_arg_index` specifies the index of the
// argument where to insert the edge when passed to the `expr_op`.
absl::StatusOr<DataSlice> SimpleAggOverEval(
    absl::string_view op_name, std::vector<DataSlice> inputs,
    internal::DataItem output_schema = internal::DataItem(),
    int edge_arg_index = 1);

// koda_internal.to_arolla_boolean operator.
//
// Attempts to cast the provided DataSlice (only rank=0 is supported) to
// boolean.
absl::StatusOr<bool> ToArollaBoolean(const DataSlice& x);

// koda_internal.to_arolla_int64 operator.
//
// Attempts to cast the provided DataSlice (only rank=0 is supported) to int64.
absl::StatusOr<int64_t> ToArollaInt64(const DataSlice& x);

// koda_internal.to_arolla_float64 operator.
//
// Attempts to cast the provided DataSlice (only rank=0 is supported) to
// float64.
absl::StatusOr<double> ToArollaFloat64(const DataSlice& x);

// koda_internal.to_arolla_dense_array_int64 operator.
//
// Attempts to cast the provided DataSlice to DenseArray<int64>.
absl::StatusOr<arolla::DenseArray<int64_t>> ToArollaDenseArrayInt64(
    const DataSlice& x);

// koda_internal.to_arolla_dense_array_unit operator.
//
// Attempts to cast the provided DataSlice to DenseArray<Unit>.
absl::StatusOr<arolla::DenseArray<arolla::Unit>> ToArollaDenseArrayUnit(
    const DataSlice& x);

// koda_internal.to_arolla_dense_array_unit operator.
//
// Attempts to cast the provided DataSlice to DenseArray<Text>.
absl::StatusOr<arolla::DenseArray<arolla::Text>> ToArollaDenseArrayText(
    const DataSlice& x);

// koda_internal._to_data_slice operator.
//
// Attempts to cast the provided value to DataSlice.
struct ToDataSliceOp {
  // Impl for Scalars.
  template <typename T>
  absl::StatusOr<DataSlice> operator()(T x) const {
    return DataSlice::Create(internal::DataItem{std::move(x)},
                             internal::DataItem{schema::GetDType<T>()});
  }

  // Impl for Optionals.
  template <typename T>
  absl::StatusOr<DataSlice> operator()(arolla::OptionalValue<T> x) const {
    return DataSlice::Create(internal::DataItem{std::move(x)},
                             internal::DataItem{schema::GetDType<T>()});
  }

  // Impl for DenseArrays.
  template <typename T>
  absl::StatusOr<DataSlice> operator()(arolla::DenseArray<T> x) const {
    auto slice_impl = internal::DataSliceImpl::Create(std::move(x));
    auto shape = DataSlice::JaggedShape::FlatFromSize(slice_impl.size());
    return DataSlice::Create(std::move(slice_impl), std::move(shape),
                             internal::DataItem{schema::GetDType<T>()});
  }
};

}  // namespace koladata::ops

#endif  // KOLADATA_OPERATORS_AROLLA_BRIDGE_H_