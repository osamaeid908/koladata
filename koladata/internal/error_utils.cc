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
#include "koladata/internal/error_utils.h"

#include <optional>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/dtype.h"
#include "koladata/internal/error.pb.h"
#include "koladata/internal/object_id.h"
#include "koladata/s11n/codec.pb.h"

namespace koladata::internal {

using ObjectIdProto = ::koladata::s11n::KodaV1Proto::ObjectIdProto;
using DataItemProto = ::koladata::s11n::KodaV1Proto::DataItemProto;

namespace {

DataItemProto EncodeObjectId(const internal::ObjectId& obj) {
  DataItemProto item_proto;
  item_proto.mutable_object_id()->set_hi(obj.InternalHigh64());
  item_proto.mutable_object_id()->set_lo(obj.InternalLow64());
  return item_proto;
}

DataItemProto EncodeDType(const schema::DType& dtype) {
  DataItemProto item_proto;
  item_proto.set_dtype(dtype.type_id());
  return item_proto;
}

}  // namespace

std::optional<Error> GetErrorPayload(const absl::Status& status) {
  auto error_payload = status.GetPayload(kErrorUrl);
  if (!error_payload) {
    return std::nullopt;
  }
  Error error;
  error.ParsePartialFromCord(*error_payload);
  return error;
}

DataItemProto EncodeSchema(const DataItem& item) {
  DCHECK(item.is_schema());
  if (item.holds_value<ObjectId>()) {
    return EncodeObjectId(item.value<ObjectId>());
  }
  return EncodeDType(item.value<schema::DType>());
}

absl::Status WithErrorPayload(absl::Status status, const Error& error) {
  if (status.ok()) {
    return status;
  }
  status.SetPayload(kErrorUrl, error.SerializePartialAsCord());
  return status;
}

absl::Status Annotate(absl::Status status, absl::string_view msg) {
  if (!status.ok()) {
    absl::Status ret_status = absl::Status(
        status.code(),
        absl::StrCat(status.message(),
                     "; Error happend when creating KodaError: ", msg));
    return ret_status;
  }
  return status;
}

}  // namespace koladata::internal
