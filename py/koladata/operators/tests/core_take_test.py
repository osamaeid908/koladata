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

import re

from absl.testing import absltest
from absl.testing import parameterized
from arolla import arolla
from koladata.exceptions import exceptions
from koladata.expr import expr_eval
from koladata.expr import input_container
from koladata.expr import view
from koladata.operators import kde_operators
from koladata.operators import optools
from koladata.operators.tests.util import qtypes as test_qtypes
from koladata.testing import testing
from koladata.types import data_slice
from koladata.types import qtypes
from koladata.types import schema_constants

I = input_container.InputContainer('I')
kde = kde_operators.kde
ds = data_slice.DataSlice.from_vals
DATA_SLICE = qtypes.DATA_SLICE


QTYPES = frozenset([(DATA_SLICE, DATA_SLICE, DATA_SLICE)])


class CoreTakeTest(parameterized.TestCase):

  @parameterized.parameters(
      # 1D DataSlice 'x'
      (ds([1, 2, 3, 4]), ds(1), ds(2)),
      (
          ds([1, 2, 3, 4]),
          ds(None, schema_constants.INT32),
          ds(None, schema_constants.INT32),
      ),
      (ds([1, 2, 3, 4]), ds([1, 3]), ds([2, 4])),
      (ds([1, 2, 3, 4]), ds([1, None]), ds([2, None])),
      (ds([1, 2, 3, 4]), ds([[1], [3]]), ds([[2], [4]])),
      (ds([1, 2, 3, 4]), ds([[1], [None]]), ds([[2], [None]])),
      # 2D DataSlice 'x'
      (ds([[1, 2], [3, 4]]), ds(1), ds([2, 4])),
      (ds([[1, 2], [3, 4]]), ds([1, 3]), ds([2, None])),
      (ds([[1, 2], [3, 4]]), ds([[1], [3]]), ds([[2], [None]])),
      # Negative indices
      (ds([[1, 2], [3, 4]]), ds(-1), ds([2, 4])),
      (ds([[1, 2], [3, 4]]), ds([-1, -2]), ds([2, 3])),
      # Out-of-bound indices
      (
          ds([[1, 2, 3], [4, 5]]),
          ds([3, -3]),
          ds([None, None], schema_constants.INT32),
      ),
      # Mixed dtypes for 'x'
      (ds([[1, '2'], ['3', 4]]), ds(1), ds(['2', 4])),
      (
          ds([[1, '2'], ['3', 4]]),
          ds([1, 3]),
          ds(['2', None], schema_constants.OBJECT),
      ),
      (
          ds([[1, '2'], ['3', 4]]),
          ds([[1], [3]]),
          ds([['2'], [None]], schema_constants.OBJECT),
      ),
      # Different index dtypes
      (ds([[1, 2], [3, 4]]), ds([1, 0], schema_constants.INT32), ds([2, 3])),
      (ds([[1, 2], [3, 4]]), ds([1, 0], schema_constants.INT64), ds([2, 3])),
      (ds([[1, 2], [3, 4]]), ds([1, 0], schema_constants.OBJECT), ds([2, 3])),
  )
  def test_eval(self, x, indices, expected):
    result = expr_eval.eval(kde.take(x, indices))
    testing.assert_equal(result, expected)

  @parameterized.parameters(
      (ds([None, None]), ds([0, 1, 0]), ds([None, None, None])),
      (
          ds([[None, None], [None, None, None]]),
          ds([[1, 0], [2]]),
          ds([[None, None], [None]]),
      ),
      (
          ds([[None, None], [None, None, None]]),
          ds([[[1, 0], [0]], [[2, 0], [0]]]),
          ds([[[None, None], [None]], [[None, None], [None]]]),
      ),
      (ds([]), ds([], schema_constants.INT32), ds([])),
  )
  def test_eval_empty_or_unknown_input(self, x, indices, expected):
    result = expr_eval.eval(kde.take(x, indices))
    testing.assert_equal(result, expected)

  def test_data_item_input_error(self):
    with self.assertRaisesRegex(
        exceptions.KodaError,
        re.escape(
            'operator kd.take failed during evaluation: DataItem is not'
            ' supported.'
        ),
    ):
      expr_eval.eval(kde.take(ds(1), ds(0)))

  def test_incompatible_shape_error(self):
    with self.assertRaisesRegex(
        exceptions.KodaError,
        re.escape(
            'operator kd.take failed during evaluation: DataSlice with'
            ' shape=JaggedShape(3) cannot be expanded to shape=JaggedShape(2);'
            ' kd.at requires shape(x)[:-1] to be broadcastable to'
            ' shape(indices) when ndim(x) <= ndim(indices)'
        ),
    ):
      expr_eval.eval(kde.take(ds([[1], [2, 3]]), ds([1, 2, 3])))

    with self.assertRaisesRegex(
        exceptions.KodaError,
        re.escape(
            """operator kd.take failed during evaluation: indices must be broadcastable to shape(x)[:-1] when ndim(x) - 1 > ndim(indices)

The cause is: DataSlice with shape=JaggedShape(3) cannot be expanded to shape=JaggedShape(2, [1, 2])"""
        ),
    ):
      expr_eval.eval(kde.take(ds([[[1]], [[2], [3]]]), ds([1, 2, 3])))

  def test_wrong_dtype_error(self):
    with self.assertRaisesRegex(
        exceptions.KodaError,
        re.escape(
            """operator kd.take failed during evaluation: invalid indices DataSlice is provided

The cause is: unsupported narrowing cast to INT64 for the given STRING DataSlice"""
        ),
    ):
      expr_eval.eval(kde.take(ds([[1], [2, 3]]), ds(['1', '2'])))

  def test_qtype_signatures(self):
    self.assertCountEqual(
        arolla.testing.detect_qtype_signatures(
            kde.core.take,
            possible_qtypes=test_qtypes.DETECT_SIGNATURES_QTYPES,
        ),
        QTYPES,
    )

  def test_view(self):
    self.assertTrue(view.has_data_slice_view(kde.take(I.x, I.y)))

  def test_alias(self):
    self.assertTrue(optools.equiv_to_op(kde.core.take, kde.take))
    self.assertTrue(optools.equiv_to_op(kde.core.at, kde.take))
    self.assertTrue(optools.equiv_to_op(kde.at, kde.take))


if __name__ == '__main__':
  absltest.main()
