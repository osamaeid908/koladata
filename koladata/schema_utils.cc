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
#include "koladata/schema_utils.h"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "absl/base/nullability.h"
#include "absl/functional/overload.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "koladata/data_slice.h"
#include "koladata/data_slice_repr.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/data_slice.h"
#include "koladata/internal/dtype.h"
#include "koladata/internal/object_id.h"
#include "koladata/internal/schema_utils.h"
#include "arolla/dense_array/dense_array.h"
#include "arolla/util/string.h"

namespace koladata {
namespace {

// A wrapper around schema::GetDType<T>().name() to handle a few special cases.
template <typename T>
constexpr absl::string_view DTypeName() {
  if constexpr (std::is_same_v<T, koladata::internal::ObjectId>) {
    // NOTE: internal::ObjectId can also mean OBJECT or SCHEMA, but for now we
    // decided to disambiguate it in the error messages.
    return "ITEMID";
  } else if constexpr (std::is_same_v<T, koladata::schema::DType>) {
    return "DTYPE";
  } else {
    return schema::GetDType<T>().name();
  }
}

}  // namespace

internal::DataItem GetNarrowedSchema(const DataSlice& slice) {
  const auto& schema = slice.GetSchemaImpl();
  if (schema == schema::kObject || schema == schema::kAny) {
    return slice.VisitImpl([&](const auto& impl) {
      if (auto data_schema = schema::GetDataSchema(impl);
          data_schema.has_value()) {
        return data_schema;
      } else {
        return schema;
      }
    });
  }
  return schema;
}

absl::Status ExpectNumeric(absl::string_view arg_name, const DataSlice& arg) {
  if (!schema::IsImplicitlyCastableTo(GetNarrowedSchema(arg),
                                      internal::DataItem(schema::kFloat64))) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "argument `%s` must be a slice of numeric values, got a slice of %s",
        arg_name, schema_utils_internal::DescribeSliceSchema(arg)));
  }
  return absl::OkStatus();
}

absl::Status ExpectInteger(absl::string_view arg_name, const DataSlice& arg) {
  if (!schema::IsImplicitlyCastableTo(GetNarrowedSchema(arg),
                                      internal::DataItem(schema::kInt64))) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "argument `%s` must be a slice of integer values, got a slice of %s",
        arg_name, schema_utils_internal::DescribeSliceSchema(arg)));
  }
  return absl::OkStatus();
}

absl::Status ExpectString(absl::string_view arg_name, const DataSlice& arg) {
  if (!schema::IsImplicitlyCastableTo(GetNarrowedSchema(arg),
                                      internal::DataItem(schema::kString))) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "argument `%s` must be a slice of %v, got a slice of %s", arg_name,
        schema::kString, schema_utils_internal::DescribeSliceSchema(arg)));
  }
  return absl::OkStatus();
}

absl::Status ExpectBytes(absl::string_view arg_name, const DataSlice& arg) {
  if (!schema::IsImplicitlyCastableTo(GetNarrowedSchema(arg),
                                      internal::DataItem(schema::kBytes))) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "argument `%s` must be a slice of %v, got a slice of %s", arg_name,
        schema::kBytes, schema_utils_internal::DescribeSliceSchema(arg)));
  }
  return absl::OkStatus();
}

absl::Status ExpectPresentScalar(absl::string_view arg_name,
                                 const DataSlice& arg,
                                 const schema::DType expected_dtype) {
  if (arg.GetShape().rank() != 0) {
    return absl::InvalidArgumentError(
        absl::StrFormat("argument `%s` must be an item holding %v, got a "
                        "slice of rank %i > 0",
                        arg_name, expected_dtype, arg.GetShape().rank()));
  }
  if (GetNarrowedSchema(arg) != expected_dtype) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "argument `%s` must be an item holding %v, got an item of %s", arg_name,
        expected_dtype, schema_utils_internal::DescribeSliceSchema(arg)));
  }
  if (arg.present_count() != 1) {
    return absl::InvalidArgumentError(
        absl::StrFormat("argument `%s` must be an item holding %v, got missing",
                        arg_name, expected_dtype));
  }
  return absl::OkStatus();
}

namespace schema_utils_internal {

std::string DescribeSliceSchema(const DataSlice& slice) {
  if (slice.GetSchemaImpl() == schema::kObject ||
      slice.GetSchemaImpl() == schema::kAny) {
    std::string result =
        absl::StrCat(slice.GetSchemaImpl(), " with ",
                     slice.size() == 1 ? "an item" : "items", " of ",
                     slice.impl_has_mixed_dtype() ? "types" : "type", " ");
    slice.VisitImpl(absl::Overload(
        [&](const internal::DataItem& impl) {
          impl.VisitValue([&]<typename T>(const T& value) {
            absl::StrAppend(&result, DTypeName<T>());
          });
        },
        [&](const internal::DataSliceImpl& impl) {
          bool first = true;
          impl.VisitValues([&]<typename T>(const arolla::DenseArray<T>& array) {
            absl::StrAppend(&result, arolla::NonFirstComma(first),
                            DTypeName<T>());
          });
        }));
    return result;
  } else {
    absl::StatusOr<std::string> schema_str = DataSliceToStr(slice.GetSchema());
    // NOTE: schema_str might be always ok(). I don't know a breaking
    // scenario, so adding the "if" just in case.
    if (!schema_str.ok()) {
      schema_str = absl::StrCat(slice.GetSchemaImpl());
    }
    return *std::move(schema_str);
  }
}

absl::Status ExpectConsistentStringOrBytesImpl(
    absl::Span<const absl::string_view> arg_names,
    absl::Span<absl::Nonnull<const DataSlice* const>> args) {
  if (args.size() != arg_names.size()) {
    return absl::InternalError("size mismatch between args and arg_names");
  }

  std::optional<size_t> string_arg_index;
  std::optional<size_t> bytes_arg_index;
  for (int i = 0; i < args.size(); ++i) {
    internal::DataItem narrowed_schema = GetNarrowedSchema(*args[i]);
    bool is_string = schema::IsImplicitlyCastableTo(
        narrowed_schema, internal::DataItem(schema::kString));
    bool is_bytes = schema::IsImplicitlyCastableTo(
        narrowed_schema, internal::DataItem(schema::kBytes));
    if (is_string && is_bytes) {
      continue;  // NONE schema.
    } else if (!is_string && !is_bytes) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "argument `%s` must be a slice of either %v or %v, got a slice of %s",
          arg_names[i], schema::kString, schema::kBytes,
          DescribeSliceSchema(*args[i])));
    } else if (is_string) {
      string_arg_index = string_arg_index.value_or(i);
    } else /* is_bytes */ {
      bytes_arg_index = bytes_arg_index.value_or(i);
    }
  }
  if (string_arg_index.has_value() && bytes_arg_index.has_value()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "mixing %v and %v arguments is not allowed, but "
        "`%s` contains %v and `%s` contains %v",
        schema::kString, schema::kBytes, arg_names[*string_arg_index],
        schema::kString, arg_names[*bytes_arg_index], schema::kBytes));
  }
  return absl::OkStatus();
}

}  // namespace schema_utils_internal
}  // namespace koladata
