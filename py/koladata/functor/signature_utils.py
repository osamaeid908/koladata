# Copyright 2024 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Tools to create and manipulate Koda functor signatures."""

import types
from koladata.functions import functions
from koladata.functor import py_functors_py_ext as _py_functors_py_ext
from koladata.types import data_slice


# Container for parameter kind constants.
ParameterKind = types.SimpleNamespace(
    POSITIONAL_ONLY=_py_functors_py_ext.positional_only_parameter_kind(),
    POSITIONAL_OR_KEYWORD=_py_functors_py_ext.positional_or_keyword_parameter_kind(),
    VAR_POSITIONAL=_py_functors_py_ext.var_positional_parameter_kind(),
    KEYWORD_ONLY=_py_functors_py_ext.keyword_only_parameter_kind(),
    VAR_KEYWORD=_py_functors_py_ext.var_keyword_parameter_kind(),
)

# The constant used to represent no default value in stored signatures.
NO_DEFAULT_VALUE = _py_functors_py_ext.no_default_value_marker()


def parameter(
    name: str,
    kind: data_slice.DataSlice,
    default_value: data_slice.DataSlice | None = None,
) -> data_slice.DataSlice:
  """Creates a functor parameter.

  Args:
    name: The name of the parameter.
    kind: The kind of the parameter, must be one of the constants from the
      ParameterKind namespace.
    default_value: The default value for the parameter. When None, no default
      value is specified (which is represented with a special constant in the
      returned DataSlice).

  Returns:
    A DataSlice with an item representing the parameter.
  """
  if default_value is None:
    default_value = NO_DEFAULT_VALUE
  return functions.obj(name=name, kind=kind, default_value=default_value)


def signature(parameters: list[data_slice.DataSlice]) -> data_slice.DataSlice:
  """Creates a functor signature.

  Note that this method does no validity checks, so the validity of the
  signature will only be checked when you try to create a functor with this
  signature.

  Args:
    parameters: The list of parameters for the signature, in order.

  Returns:
    A DataSlice representing the signature.
  """
  return functions.obj(parameters=functions.list(parameters))