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

"""User-facing module for Koda external libraries related APIs."""

from koladata import kd as _kd
from koladata.ext import nested_data as _nested_data
from koladata.ext import npkd as _npkd
from koladata.ext import pdkd as _pdkd

npkd = _npkd
pdkd = _pdkd
nested_data = _nested_data

# CamelCase versions in ext since we're still not sure what we will eventually
# recommend, and 'kd' is consistently lowercase for all operations.
Fn = _kd.fn
PyFn = _kd.py_fn
