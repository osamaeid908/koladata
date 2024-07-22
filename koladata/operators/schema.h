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
#ifndef KOLADATA_OPERATORS_SCHEMA_H_
#define KOLADATA_OPERATORS_SCHEMA_H_

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "koladata/data_slice.h"
#include "koladata/data_slice_qtype.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/dtype.h"
#include "arolla/util/status_macros_backport.h"
#include "absl/types/span.h"
#include "arolla/qexpr/operators.h"
#include "arolla/qtype/qtype.h"

namespace koladata::ops {

// kde.schema.new_schema operator.
// Creates a new allocated schema.
class NewSchemaOperatorFamily : public arolla::OperatorFamily {
  absl::StatusOr<arolla::OperatorPtr> DoGetOperator(
      absl::Span<const arolla::QTypePtr> input_types,
      arolla::QTypePtr output_type) const final;
};

// kde.core.get_primitive_schema.
inline absl::StatusOr<DataSlice> GetPrimitiveSchema(const DataSlice& ds) {
  if (!schema::DType::VerifyQTypeSupported(ds.dtype())) {
    return absl::FailedPreconditionError(
        "DataSlice does not have any items or has non-primitive items or has "
        "items of mixed primitive dtypes.");
  }
  ASSIGN_OR_RETURN(auto dtype, schema::DType::FromQType(ds.dtype()));
  return DataSlice::Create(internal::DataItem(dtype),
                           internal::DataItem(schema::kSchema));
}

// kde.schema._cast_to operator.
absl::StatusOr<DataSlice> CastTo(const DataSlice& x, const DataSlice& schema,
                                 const DataSlice& implicit_cast);

// kde.schema.with_schema operator.
inline absl::StatusOr<DataSlice> WithSchema(const DataSlice& ds,
                                            const DataSlice& schema) {
  return ds.WithSchema(schema);
}

}  // namespace koladata::ops

#endif  // KOLADATA_OPERATORS_SCHEMA_H_
