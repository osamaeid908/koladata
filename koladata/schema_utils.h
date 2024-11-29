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
#ifndef KOLADATA_SCHEMA_UTILS_H_
#define KOLADATA_SCHEMA_UTILS_H_

#include <string>

#include "absl/base/nullability.h"
#include "absl/status/status.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "koladata/data_slice.h"
#include "koladata/internal/data_item.h"

namespace koladata {

// Returns the common schema of the underlying data. If the schema is ambiguous
// (e.g. the slice holds ObjectIds, or the data is mixed but there is no common
// type), the schema of the original slice is returned.
//
// Example:
//  * GetNarrowedSchema(kd.slice([1])) -> INT32.
//  * GetNarrowedSchema(kd.slice([1, 2.0], OBJECT)) -> FLOAT32.
//  * GetNarrowedSchema(kd.slice([None, None], OBJECT)) -> NONE.
internal::DataItem GetNarrowedSchema(const DataSlice& slice);

// Returns OK if the DataSlice's schema is a numeric type or narrowed to it.
absl::Status ExpectNumeric(absl::string_view arg_name, const DataSlice& arg);

// Returns OK if the DataSlice contains a scalar boolean value.
absl::Status ExpectScalarBool(absl::string_view arg_name, const DataSlice& arg);

namespace schema_utils_internal {

// (internal) Implementation of ExpectConsistentStringOrBytes.
absl::Status ExpectConsistentStringOrBytesImpl(
    absl::Span<const absl::string_view> arg_names,
    absl::Span<absl::Nonnull<const DataSlice* const>> args);

// (internal) Returns a human-readable description of the schema of the
// DataSlice. The function is public only for testing.
std::string DescribeSliceSchema(const DataSlice& slice);

}  // namespace schema_utils_internal

// Returns OK if the DataSlices' schemas are all strings or byteses, and they
// are not mixed.
template <typename... DataSlices>
absl::Status ExpectConsistentStringOrBytes(
    absl::Span<const absl::string_view> arg_names, const DataSlices&... args) {
  return schema_utils_internal::ExpectConsistentStringOrBytesImpl(arg_names,
                                                                  {&args...});
}

}  // namespace koladata

#endif  // KOLADATA_SCHEMA_UTILS_H_
