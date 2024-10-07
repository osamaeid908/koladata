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

"""Tests for kde.py.map_py_on_selected operator."""

from absl.testing import absltest
from absl.testing import parameterized
from koladata.expr import expr_eval
from koladata.expr import input_container
from koladata.expr import view
from koladata.operators import kde_operators
from koladata.operators import optools
from koladata.testing import testing
from koladata.types import data_slice

I = input_container.InputContainer("I")
ds = data_slice.DataSlice.from_vals
kde = kde_operators.kde


class PyMapPyOnSelectedTest(parameterized.TestCase):

  # Note: This operator is assembled from the same building blocks as
  # the operator `kde.py.map_py`, and these are tested together with that
  # operator.

  def test_args_kwargs(self):
    x = ds([1, 2, None])
    y = ds([3, None, 4])
    r = expr_eval.eval(kde.py.map_py_on_present(lambda x, y: x + y, x, y))
    testing.assert_equal(r.no_db(), ds([4, None, None]))

    r = expr_eval.eval(kde.py.map_py_on_present(lambda x, y: x + y, x, y=y))
    testing.assert_equal(r.no_db(), ds([4, None, None]))

    r = expr_eval.eval(kde.py.map_py_on_present(lambda x, y: x + y, x=x, y=y))
    testing.assert_equal(r.no_db(), ds([4, None, None]))

  def test_rank_2(self):
    x = ds([[1, 2], [], []])
    y = ds([3.5, None, 4.5])
    r = expr_eval.eval(kde.py.map_py_on_present(lambda x, y: x + y, x, y))
    testing.assert_equal(r.no_db(), ds([[4.5, 5.5], [], []]))

  def test_view(self):
    self.assertTrue(
        view.has_data_slice_view(kde.py.map_py_on_present(I.fn, I.cond, I.arg))
    )

  def test_alias(self):
    self.assertTrue(
        optools.equiv_to_op(kde.py.map_py_on_present, kde.map_py_on_present)
    )


if __name__ == "__main__":
  absltest.main()