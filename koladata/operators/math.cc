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
#include "koladata/operators/math.h"

#include <memory>

#include "absl/status/statusor.h"
#include "koladata/data_slice.h"
#include "koladata/operators/convert_and_eval.h"
#include "arolla/expr/registered_expr_operator.h"

namespace koladata::ops {

absl::StatusOr<DataSlice> Subtract(const DataSlice& x, const DataSlice& y) {
  return SimplePointwiseEval(
      std::make_shared<arolla::expr::RegisteredOperator>("math.subtract"),
      {x, y});
}

}  // namespace koladata::ops