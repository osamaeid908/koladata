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

"""Tests for kde.comparison.less."""

from absl.testing import absltest
from absl.testing import parameterized
from arolla import arolla
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


QTYPES = frozenset([
    (DATA_SLICE, DATA_SLICE, DATA_SLICE),
])


class ComparisonLessTest(parameterized.TestCase):

  @parameterized.parameters(
      (
          ds([1, 3, 2]),
          ds([1, 2, 3]),
          ds([None, None, arolla.present()]),
      ),
      (
          ds([1, 3, 2], schema_constants.FLOAT32),
          ds([1, 2, 3], schema_constants.FLOAT32),
          ds([None, None, arolla.present()]),
      ),
      # Auto-broadcasting
      (
          ds([1, 2, 3], schema_constants.FLOAT32),
          ds(2, schema_constants.FLOAT32),
          ds([arolla.present(), None, None]),
      ),
      # scalar inputs, scalar output.
      (3, 4, ds(arolla.present())),
      (4, 3, ds(None, schema_constants.MASK)),
      # multi-dimensional.
      (
          ds([1, 2, 3]),
          ds([[0, 1, 2], [1, 2, 3], [2, 3, 4]]),
          ds([[None, None, arolla.present()]] * 3),
      ),
      # OBJECT/ANY
      (
          ds([1, None, 5], schema_constants.OBJECT),
          ds([4, 1, 0], schema_constants.ANY),
          ds([arolla.present(), None, None]),
      ),
      # Empty and unknown inputs.
      (
          ds([None, None, None], schema_constants.OBJECT),
          ds([None, None, None], schema_constants.OBJECT),
          ds([None, None, None], schema_constants.MASK),
      ),
      (
          ds([None, None, None]),
          ds([None, None, None]),
          ds([None, None, None], schema_constants.MASK),
      ),
      (
          ds([None, None, None]),
          ds([None, None, None], schema_constants.FLOAT32),
          ds([None, None, None], schema_constants.MASK),
      ),
      (
          ds([None, None, None], schema_constants.INT32),
          ds([None, None, None], schema_constants.FLOAT32),
          ds([None, None, None], schema_constants.MASK),
      ),
      (
          ds([None, None, None], schema_constants.ANY),
          ds([None, None, None], schema_constants.FLOAT32),
          ds([None, None, None], schema_constants.MASK),
      ),
      (
          ds([None, None, None]),
          ds([4, 1, 0]),
          ds([None, None, None], schema_constants.MASK),
      ),
      (
          ds([None, None, None], schema_constants.ANY),
          ds([4, 1, 0]),
          ds([None, None, None], schema_constants.MASK),
      ),
  )
  def test_eval(self, x, y, expected):
    result = expr_eval.eval(kde.comparison.less(I.x, I.y), x=x, y=y)
    testing.assert_equal(result, expected)

  def test_qtype_difference(self):
    x = data_slice.DataSlice.from_vals([1, 2, 3])
    y = data_slice.DataSlice.from_vals(['a', 'b', 'c'])
    with self.assertRaisesRegex(
        ValueError,
        'incompatible types',
    ):
      expr_eval.eval(kde.comparison.less(I.x, I.y), x=x, y=y)

  def test_qtype_signatures(self):
    self.assertCountEqual(
        arolla.testing.detect_qtype_signatures(
            kde.comparison.less,
            possible_qtypes=test_qtypes.DETECT_SIGNATURES_QTYPES,
        ),
        QTYPES,
    )

  def test_repr(self):
    self.assertEqual(repr(kde.comparison.less(I.x, I.y)), 'I.x < I.y')
    self.assertEqual(repr(kde.less(I.x, I.y)), 'I.x < I.y')

  def test_view(self):
    self.assertTrue(view.has_data_slice_view(kde.comparison.less(I.x, I.y)))

  def test_alias(self):
    self.assertTrue(optools.equiv_to_op(kde.comparison.less, kde.less))


if __name__ == '__main__':
  absltest.main()