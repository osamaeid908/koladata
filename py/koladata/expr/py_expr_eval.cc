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
#include "py/koladata/expr/py_expr_eval.h"

#include <Python.h>  // IWYU pragma: keep

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "absl/base/no_destructor.h"
#include "absl/base/nullability.h"
#include "absl/log/check.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "koladata/data_slice.h"
#include "koladata/data_slice_qtype.h"
#include "koladata/expr/constants.h"
#include "koladata/expr/expr_eval.h"
#include "py/arolla/abc/py_aux_binding_policy.h"
#include "py/arolla/abc/py_cached_eval.h"
#include "py/arolla/abc/py_expr.h"
#include "py/arolla/abc/py_operator.h"
#include "py/arolla/abc/py_qvalue.h"
#include "py/arolla/abc/py_qvalue_specialization.h"
#include "py/arolla/py_utils/py_utils.h"
#include "py/koladata/types/py_utils.h"
#include "py/koladata/types/wrap_utils.h"
#include "arolla/expr/expr_node.h"
#include "arolla/expr/expr_operator_signature.h"
#include "arolla/expr/registered_expr_operator.h"
#include "arolla/qtype/typed_ref.h"
#include "arolla/qtype/typed_value.h"
#include "arolla/util/status_macros_backport.h"

namespace koladata::python {

using ::arolla::TypedRef;
using ::arolla::TypedValue;
using ::arolla::expr::ExprNodePtr;
using ::arolla::expr::IsRegisteredOperator;
using ::arolla::python::AuxBindArguments;
using ::arolla::python::AuxBindingPolicyPtr;
using ::arolla::python::DCheckPyGIL;
using ::arolla::python::InvokeOpWithCompilationCache;
using ::arolla::python::ParseArgPyOperator;
using ::arolla::python::QValueOrExpr;
using ::arolla::python::ReleasePyGIL;
using ::arolla::python::SetPyErrFromStatus;
using ::arolla::python::UnwrapPyExpr;
using ::arolla::python::UnwrapPyQValue;
using ::arolla::python::WrapAsPyQValue;

absl::Nullable<PyObject*> PyEvalExpr(PyObject* /*self*/, PyObject** py_args,
                                     Py_ssize_t nargs, PyObject* py_kwnames) {
  DCheckPyGIL();
  static const absl::NoDestructor<FastcallArgParser> parser(
      /*pos_only_n=*/1, /*parse_kwargs=*/true);
  FastcallArgParser::Args args;
  if (!parser->Parse(py_args, nargs, py_kwnames, args)) {
    return nullptr;
  }

  // Parse expr.
  DCHECK_GE(nargs, 1);  // Checked above in FastcallArgParser::Parse.
  auto expr = UnwrapPyExpr(py_args[0]);
  if (expr == nullptr) {
    PyErr_Clear();
    return PyErr_Format(PyExc_TypeError,
                        "kd.eval() expects an expression, got expr: %s",
                        Py_TYPE(py_args[0])->tp_name);
  }

  // Parse inputs.
  std::vector<std::pair<std::string, TypedRef>> input_qvalues;
  input_qvalues.reserve(args.kw_names.size());
  for (int i = 0; i < args.kw_names.size(); ++i) {
    const auto* typed_value = UnwrapPyQValue(args.kw_values[i]);
    if (typed_value == nullptr) {
      PyErr_Clear();
      return PyErr_Format(
          PyExc_TypeError,
          "kd.eval() expects all inputs to be QValues, got: %s=%s",
          std::string(args.kw_names[i]).c_str(),
          Py_TYPE(args.kw_values[i])->tp_name);
    }
    input_qvalues.emplace_back(args.kw_names[i], typed_value->AsRef());
  }

  // Evaluate the expression.
  absl::StatusOr<TypedValue> result_or_error;
  {
    // We leave the Python world here, so we no longer need the GIL.
    ReleasePyGIL guard;
    result_or_error =
        koladata::expr::EvalExprWithCompilationCache(expr, input_qvalues, {});
  }
  ASSIGN_OR_RETURN(auto result, std::move(result_or_error),
                   SetPyErrFromStatus(_));
  return WrapAsPyQValue(std::move(result));
}

PyObject* PyUnspecifiedSelfInput(PyObject* /*self*/, PyObject* /*py_args*/) {
  DCheckPyGIL();
  // We make a copy since WrapPyDataSlice takes ownership.
  DataSlice unspecified_self_input = expr::UnspecifiedSelfInput();
  return WrapPyDataSlice(std::move(unspecified_self_input));
}

PyObject* PyClearEvalCache(PyObject* /*self*/, PyObject* /*py_args*/) {
  DCheckPyGIL();
  {
    ReleasePyGIL guard;
    koladata::expr::ClearCompilationCache();
  }
  Py_RETURN_NONE;
}

absl::Nullable<PyObject*> PyEvalOp(PyObject* /*self*/, PyObject** py_args,
                                   Py_ssize_t nargs, PyObject* py_kwnames) {
  DCheckPyGIL();
  if (nargs == 0) {
    PyErr_SetString(
        PyExc_TypeError,
        "kd.eval_op() missing 1 required positional argument: 'op'");
    return nullptr;
  }

  // Parse the operator.
  auto op = ParseArgPyOperator("kd.eval_op", py_args[0]);
  if (op == nullptr) {
    return nullptr;
  }

  // Bind the arguments.
  ASSIGN_OR_RETURN(auto signature, op->GetSignature(), SetPyErrFromStatus(_));
  std::vector<QValueOrExpr> bound_args;
  AuxBindingPolicyPtr policy_implementation;
  if (!AuxBindArguments(signature, py_args + 1,
                        (nargs - 1) | PY_VECTORCALL_ARGUMENTS_OFFSET,
                        py_kwnames, &bound_args, &policy_implementation)) {
    return nullptr;
  }

  // Generate `input_qvalues`.
  const auto param_name = [&signature](size_t i) -> std::string {
    if (!HasVariadicParameter(signature)) {
      DCHECK_LT(i, signature.parameters.size());
      return signature.parameters[i].name;
    }
    if (i + 1 < signature.parameters.size()) {
      return signature.parameters[i].name;
    }
    return absl::StrCat(signature.parameters.back().name, "[",
                        i - signature.parameters.size() + 1, "]");
  };

  std::vector<TypedRef> input_qvalues;
  std::vector<TypedValue> holder;
  input_qvalues.reserve(bound_args.size());
  for (size_t i = 0; i < bound_args.size(); ++i) {
    if (auto* qvalue = std::get_if<TypedValue>(&bound_args[i])) {
      input_qvalues.push_back(qvalue->AsRef());
      continue;
    }
    const auto& expr = *std::get_if<ExprNodePtr>(&bound_args[i]);
    if (IsRegisteredOperator(expr->op()) &&
        expr->op()->display_name() == "math.add" &&
        expr->node_deps().size() == 2 &&
        expr->node_deps()[0]->leaf_key() == "_koladata_hidden_seed_leaf") {
      holder.push_back(TypedValue::FromValue<int64_t>(static_cast<int64_t>(i)));
      input_qvalues.push_back(holder.back().AsRef());
      continue;
    }
    return PyErr_Format(PyExc_TypeError,
                        "kd.eval_op() expected all arguments to be values, "
                        "got an expression for the parameter '%s'",
                        param_name(i).c_str());
  }

  // Call the implementation.
  ASSIGN_OR_RETURN(auto result,
                   InvokeOpWithCompilationCache(std::move(op), input_qvalues),
                   SetPyErrFromStatus(_));
  return WrapAsPyQValue(std::move(result));
}

}  // namespace koladata::python
