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
#ifndef KOLADATA_INTERNAL_OP_UTILS_AGG_UUID_H_
#define KOLADATA_INTERNAL_OP_UTILS_AGG_UUID_H_

#include "absl/status/statusor.h"
#include "koladata/internal/data_slice.h"
#include "arolla/jagged_shape/dense_array/jagged_shape.h"

namespace koladata::internal {

absl::StatusOr<DataSliceImpl> AggUuidOp(
    const DataSliceImpl& ds, const arolla::JaggedDenseArrayShape& shape);

}  // namespace koladata::internal


#endif  // KOLADATA_INTERNAL_OP_UTILS_AGG_UUID_H_
