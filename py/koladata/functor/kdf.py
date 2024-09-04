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

"""User-facing module for Koda functor related APIs."""

from koladata.functor import functor_factories as _functor_factories
from koladata.operators import eager_op_utils as _eager_op_utils

_kd = _eager_op_utils.operators_container('kde')

fn = _functor_factories.fn
is_fn = _functor_factories.is_fn
call = _kd.call