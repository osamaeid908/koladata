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
#include "koladata/arolla_utils.h"

#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "koladata/data_slice.h"
#include "koladata/data_slice_qtype.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/data_slice.h"
#include "koladata/internal/dtype.h"
#include "koladata/internal/missing_value.h"
#include "koladata/internal/object_id.h"
#include "koladata/internal/types.h"
#include "arolla/array/array.h"
#include "arolla/array/qtype/types.h"
#include "arolla/dense_array/dense_array.h"
#include "arolla/dense_array/qtype/types.h"
#include "arolla/memory/optional_value.h"
#include "arolla/qtype/base_types.h"
#include "arolla/qtype/qtype.h"
#include "arolla/qtype/qtype_traits.h"
#include "arolla/qtype/typed_ref.h"
#include "arolla/qtype/typed_value.h"
#include "arolla/util/meta.h"
#include "arolla/util/status_macros_backport.h"

namespace koladata {

absl::StatusOr<arolla::TypedValue> DataSliceToArollaValue(
    const DataSlice& ds, const internal::DataItem& fallback_schema) {
  if (ds.impl_owns_value()) {
    // In this case, DataSlice owns underlying DenseArray / scalar value which
    // can be returned through TypedRef.
    ASSIGN_OR_RETURN(auto ref, DataSliceToArollaRef(ds));
    return arolla::TypedValue(ref);
  }
  // The following code creates empty Arolla value with appropriate type if
  // possible. Otherwise returns appropriate error.
  //
  // NONE, OBJECT, ANY need to get a primitive schema.
  auto schema_item = ds.GetSchemaImpl();
  if (!schema_item.is_entity_schema() && !schema_item.is_primitive_schema()) {
    schema_item = fallback_schema;
  }
  if (!schema_item.is_primitive_schema()) {
    return absl::FailedPreconditionError(
        "empty slices can be converted to Arolla value only if they have "
        "primitive schema");
  }
  std::optional<arolla::TypedValue> result;
  arolla::meta::foreach_type(
      schema::supported_primitive_dtypes(), [&](auto tpe) {
        using SchemaT = typename decltype(tpe)::type;
        if (schema_item.value<schema::DType>() == schema::GetDType<SchemaT>()) {
          result = ds.VisitImpl([&]<class T>(const T& impl) {
            if constexpr (std::is_same_v<T, internal::DataItem>) {
              return arolla::TypedValue::FromValue(
                  arolla::OptionalValue<SchemaT>{});
            } else {
              return arolla::TypedValue::FromValue(
                  arolla::CreateEmptyDenseArray<SchemaT>(impl.size()));
            }
          });
        }
      });
  DCHECK(result.has_value());
  return *std::move(result);
}

absl::StatusOr<arolla::TypedRef> DataSliceToArollaRef(const DataSlice& ds) {
  DCHECK(ds.impl_owns_value());
  if (ds.impl_has_mixed_dtype()) {
    return absl::FailedPreconditionError(
        "only DataSlices with primitive values of the same type can be "
        "converted to Arolla value, got: MIXED");
  }
  std::optional<arolla::TypedRef> result;
  ds.VisitImpl([&]<class T>(const T& impl) {
    auto to_ref = [&](const auto& val) {
      using ValT = typename std::decay_t<decltype(val)>;
      if constexpr (!std::is_same_v<ValT, internal::MissingValue> &&
                    !std::is_same_v<ValT, internal::ObjectId> &&
                    !std::is_same_v<ValT,
                                    arolla::DenseArray<internal::ObjectId>>) {
        result = arolla::TypedRef::FromValue(val);
      }
    };
    if constexpr (std::is_same_v<T, internal::DataItem>) {
      impl.VisitValue(to_ref);
    } else {
      impl.VisitValues(to_ref);
    }
  });
  if (!result.has_value()) {
    const auto& schema_item = ds.GetSchemaImpl();
    return absl::FailedPreconditionError(absl::StrCat(
        "unsupported dtype for conversions to Arolla value: ",
        schema_item.holds_value<schema::DType>() ?
        schema_item.value<schema::DType>().name() : "OBJECT_ID"));
  }
  return *std::move(result);
}

absl::StatusOr<DataSlice> DataSliceFromPrimitivesDenseArray(
    arolla::TypedRef values) {
  if (!arolla::IsDenseArrayQType(values.GetType())) {
    return absl::InvalidArgumentError(
        absl::StrCat("expected DenseArray, but got: ",
                     values.GetType()->name()));
  }
  ASSIGN_OR_RETURN(auto ds_impl, internal::DataSliceImpl::Create(values));
  size_t size = ds_impl.size();
  return DataSlice::CreateWithSchemaFromData(
      std::move(ds_impl), DataSlice::JaggedShape::FlatFromSize(size));
}

absl::StatusOr<DataSlice> DataSliceFromPrimitivesArray(
    arolla::TypedRef values) {
  if (!arolla::IsArrayQType(values.GetType())) {
    return absl::InvalidArgumentError(
        absl::StrCat("expected Arolla Array, but got: ",
                     values.GetType()->name()));
  }
  std::optional<absl::StatusOr<DataSlice>> res;
  arolla::meta::foreach_type(
      internal::supported_primitives_list(), [&](auto tpe) {
        using T = typename decltype(tpe)::type;
        if (values.GetType()->value_qtype() == arolla::GetQType<T>()) {
          auto ds_impl = internal::DataSliceImpl::Create(
              values.UnsafeAs<arolla::Array<T>>().ToDenseForm().dense_data()
              .ForceNoBitmapBitOffset());
          size_t size = ds_impl.size();
          res = DataSlice::CreateWithSchemaFromData(
              std::move(ds_impl), DataSlice::JaggedShape::FlatFromSize(size));
        }
      });
  if (!res.has_value()) {
    return absl::InvalidArgumentError(
        absl::StrCat("unsupported array element type: ",
                     values.GetType()->value_qtype()->name()));
  }
  return *std::move(res);
}

absl::StatusOr<arolla::TypedValue> DataSliceToDenseArray(const DataSlice& ds) {
  if (ds.GetShape().rank() == 0) {
    internal::DataSliceImpl::Builder bldr(1);
    bldr.Insert(0, ds.item());
    ASSIGN_OR_RETURN(
        auto flat_ds,
        DataSlice::Create(std::move(bldr).Build(),
                          DataSlice::JaggedShape::FlatFromSize(1),
                          ds.GetSchemaImpl()));
    // NOTE: ToArollaValue returns DenseArray only for multi-dim slices.
    return DataSliceToArollaValue(flat_ds);
  }
  return DataSliceToArollaValue(ds);
}

}  // namespace koladata
