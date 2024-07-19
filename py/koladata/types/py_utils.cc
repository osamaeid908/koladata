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
#include "py/koladata/types/py_utils.h"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/string_view.h"
#include "koladata/adoption_utils.h"
#include "koladata/data_bag.h"
#include "koladata/data_slice.h"
#include "koladata/data_slice_qtype.h"
#include "koladata/object_factories.h"
#include "py/arolla/abc/py_qvalue.h"
#include "py/koladata/types/boxing.h"
#include "arolla/qtype/qtype_traits.h"
#include "arolla/util/status_macros_backport.h"

namespace koladata::python {

namespace {

// Returns true if there is at least one DataSlice that is not DataItem, i.e.
// its rank is >0.
bool HasNonItemDataSlice(const std::vector<PyObject*>& args) {
  for (PyObject* arg : args) {
    if (!arolla::python::IsPyQValueInstance(arg)) {
      continue;
    }
    if (auto tv = arolla::python::UnsafeUnwrapPyQValue(arg);
        tv.GetType() == arolla::GetQType<DataSlice>()) {
      const DataSlice& ds = tv.UnsafeAs<DataSlice>();
      if (ds.GetShape().rank() > 0) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

absl::StatusOr<DataSlice> AssignmentRhsFromPyValue(
    PyObject* rhs, bool prohibit_boxing_to_multi_dim_slice,
    const DataBagPtr& db, AdoptionQueue& adoption_queue) {
  // Short-circuit the most common case.
  if (arolla::python::IsPyQValueInstance(rhs)) {
    return DataSliceFromPyValue(rhs, adoption_queue);
  }
  if (PyDict_Check(rhs)) {
    if (prohibit_boxing_to_multi_dim_slice) {
      return absl::InvalidArgumentError(
          "assigning a Python dict to an attribute is only supported"
          " for Koda Dict DataItem, but not for 1+-dimensional slices."
          " use kd.dict() if you want to create the same"
          " dictionary instance to be assigned to all items in"
          " the slice, or kd.dict_like() to create multiple"
          " dictionary instances");
    }
    return EntitiesFromPyObject(rhs, db, adoption_queue);
  }
  ASSIGN_OR_RETURN(auto res, DataSliceFromPyValue(rhs, adoption_queue));
  if (res.GetShape().rank() > 0) {
    // NOTE: `rhs` is not a DataSlice and is a Python iterable / sequence that
    // `DataSliceFromPyValue` treats as multidimensional (e.g. lists and lists,
    // but not `str` or `bytes`).
    if (prohibit_boxing_to_multi_dim_slice) {
      return absl::InvalidArgumentError(
          "assigning a Python list/tuple to an attribute is only supported"
          " for Koda List DataItem, but not for 1+-dimensional slices."
          " use kd.list() if you want to create the same"
          " list instance to be assigned to all items in"
          " the slice, kd.list_like() to create multiple"
          " list instances, or kd.slice() to create a slice");
    }
    return CreateNestedList(db, res, /*schema=*/std::nullopt);
  }
  return std::move(res);
}

absl::StatusOr<DataSlice> AssignmentRhsFromPyValue(
    const DataSlice& lhs_ds, PyObject* rhs, AdoptionQueue& adoption_queue) {
  return AssignmentRhsFromPyValue(rhs, lhs_ds.GetShape().rank() != 0,
                                  lhs_ds.GetDb(), adoption_queue);
}

absl::StatusOr<std::vector<DataSlice>> UnwrapDataSlices(
    const std::vector<PyObject*>& args, AdoptionQueue& adoption_queue) {
  std::vector<DataSlice> values;
  values.reserve(args.size());
  for (PyObject* arg : args) {
    if (arolla::python::IsPyQValueInstance(arg)) {
      const auto& typed_value = arolla::python::UnsafeUnwrapPyQValue(arg);
      if (typed_value.GetType() != arolla::GetQType<DataSlice>()) {
        return absl::InvalidArgumentError(
            absl::StrFormat("expected DataSlice argument, got %s",
                            Py_TYPE(arg)->tp_name));
      }
      const DataSlice& ds_arg = typed_value.UnsafeAs<DataSlice>();
      adoption_queue.Add(ds_arg);
      values.push_back(std::move(ds_arg));
    } else {
      return absl::InvalidArgumentError(
          absl::StrFormat("expected DataSlice argument, got %s",
                          Py_TYPE(arg)->tp_name));
    }
  }
  return values;
}

absl::StatusOr<std::vector<DataSlice>> ConvertArgsToDataSlices(
    const DataBagPtr& db, const std::vector<PyObject*>& args,
    AdoptionQueue& adoption_queue) {
  return ConvertArgsToDataSlices(db, HasNonItemDataSlice(args), args,
                                 adoption_queue);
}

absl::StatusOr<std::vector<DataSlice>> ConvertArgsToDataSlices(
    const DataBagPtr& db, bool prohibit_boxing_to_multi_dim_slice,
    const std::vector<PyObject*>& args, AdoptionQueue& adoption_queue) {
  std::vector<DataSlice> values;
  values.reserve(args.size());
  for (PyObject* arg : args) {
    ASSIGN_OR_RETURN(
        auto value,
        AssignmentRhsFromPyValue(arg, prohibit_boxing_to_multi_dim_slice, db,
                                 adoption_queue));
    values.push_back(std::move(value));
  }
  return std::move(values);
}

bool ParseUpdateSchemaArg(const FastcallArgParser::Args& args, size_t arg_pos,
                          bool& update_schema) {
  // args.pos_kw_values[arg_pos] is "update_schema" optional positional-keyword
  // argument.
  if (args.pos_kw_values.size() <= arg_pos ||
      args.pos_kw_values[arg_pos] == nullptr ||
      args.pos_kw_values[arg_pos] == Py_None) {
    update_schema = false;
    return true;
  }
  if (!PyBool_Check(args.pos_kw_values[arg_pos])) {
    PyErr_Format(PyExc_TypeError, "expected bool for update_schema, got %s",
                 Py_TYPE(args.pos_kw_values[arg_pos])->tp_name);
    return false;
  }
  update_schema = PyObject_IsTrue(args.pos_kw_values[arg_pos]);
  return true;
}

namespace {

// Sets Python TypeError if inadequate number of positional arguments have been
// passed to a function / method.
bool InvalidPosArgCountError(Py_ssize_t nargs, size_t pos_only_n,
                                size_t pos_keyword_n) {
  if (pos_only_n > 0 || pos_keyword_n > 0) {
    PyErr_Format(PyExc_TypeError,
                 "accepts %d to %d positional arguments but %d %s given",
                 pos_only_n, pos_only_n + pos_keyword_n, nargs,
                 nargs == 1 ? "was" : "were");
  } else {
    PyErr_Format(PyExc_TypeError,
                 "accepts 0 positional arguments but %d %s given",
                 nargs, nargs == 1 ? "was" : "were");
  }
  return false;
}

}  // namespace

bool FastcallArgParser::Parse(PyObject* const* py_args, Py_ssize_t nargs,
                              PyObject* py_kwnames, Args& args) const {
  if (parse_kwargs_) {
    args.kw_names.reserve(kKwargsVectorCapacity);
    args.kw_values.reserve(kKwargsVectorCapacity);
  }
  args.pos_kw_values.assign(pos_kw_to_pos_.size(), nullptr);
  if (pos_only_n_ > nargs) {
    // The caller should process all missing positional-keyword arguments.
    return true;
  }
  if (nargs > pos_kw_to_pos_.size() + pos_only_n_) {
    return InvalidPosArgCountError(nargs, pos_only_n_, pos_kw_to_pos_.size());
  }
  for (size_t i = pos_only_n_; i < nargs; ++i) {
    args.pos_kw_values[i - pos_only_n_] = py_args[i];
  }
  if (py_kwnames == nullptr) {
    return true;
  }
  Py_ssize_t kwargs_size = PyTuple_GET_SIZE(py_kwnames);
  for (Py_ssize_t i = 0; i < kwargs_size; ++i) {
    PyObject* const py_key = PyTuple_GET_ITEM(py_kwnames, i);
    Py_ssize_t key_size = 0;
    // NOTE: The underlying UTF8 is cached in Unicode object and the pointer
    // to the same buffer is returned on multiple calls. This means it is safe
    // to use these pointers as long as Unicode object is alive:
    // https://docs.python.org/3/c-api/unicode.html
    const char* key_data = PyUnicode_AsUTF8AndSize(py_key, &key_size);
    if (key_data == nullptr) {
      return false;
    }
    absl::string_view arg_name(key_data, key_size);
    auto arg_pos_it = pos_kw_to_pos_.find(
        absl::string_view(key_data, key_size));
    if (arg_pos_it == pos_kw_to_pos_.end()) {
      if (parse_kwargs_) {
        args.kw_names.push_back(arg_name);
        args.kw_values.push_back(py_args[nargs + i]);
        continue;
      }
      PyErr_Format(PyExc_TypeError, "got an unexpected keyword %R", py_key);
      return false;
    }
    if (args.pos_kw_values[arg_pos_it->second] != nullptr) {
      PyErr_Format(PyExc_TypeError,
                   "got multiple values for argument %R", py_key);
      return false;
    }
    args.pos_kw_values[arg_pos_it->second] = py_args[nargs + i];
  }
  return true;
}

}  // namespace koladata::python
