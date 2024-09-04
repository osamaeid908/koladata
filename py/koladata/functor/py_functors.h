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
#ifndef THIRD_PARTY_PY_KOLADATA_FUNCTOR_PY_FUNCTORS_H_
#define THIRD_PARTY_PY_KOLADATA_FUNCTOR_PY_FUNCTORS_H_

#include <Python.h>

#include "absl/base/nullability.h"

namespace koladata::python {

absl::Nullable<PyObject*> PyPositionalOnlyParameterKind(PyObject* /*self*/,
                                                        PyObject* /*py_args*/);
absl::Nullable<PyObject*> PyPositionalOrKeywordParameterKind(
    PyObject* /*self*/, PyObject* /*py_args*/);
absl::Nullable<PyObject*> PyVarPositionalParameterKind(PyObject* /*self*/,
                                                       PyObject* /*py_args*/);
absl::Nullable<PyObject*> PyKeywordOnlyParameterKind(PyObject* /*self*/,
                                                     PyObject* /*py_args*/);
absl::Nullable<PyObject*> PyVarKeywordParameterKind(PyObject* /*self*/,
                                                    PyObject* /*py_args*/);

absl::Nullable<PyObject*> PyNoDefaultValueMarker(PyObject* /*self*/,
                                                 PyObject* /*py_args*/);

absl::Nullable<PyObject*> PyCreateFunctor(PyObject* /*self*/,
                                          PyObject** py_args, Py_ssize_t nargs,
                                          PyObject* py_kwnames);

absl::Nullable<PyObject*> PyIsFn(PyObject* /*self*/, PyObject* fn);
}  // namespace koladata::python

#endif  // THIRD_PARTY_PY_KOLADATA_FUNCTOR_PY_FUNCTORS_H_