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

"""Tests for kde.strings.upper."""

from absl.testing import absltest
from absl.testing import parameterized
from arolla import arolla
from koladata.expr import expr_eval
from koladata.expr import input_container
from koladata.expr import view
from koladata.operators import kde_operators
from koladata.operators.tests.util import qtypes as test_qtypes
from koladata.testing import testing
from koladata.types import data_item
from koladata.types import data_slice
from koladata.types import qtypes
from koladata.types import schema_constants

I = input_container.InputContainer('I')
kde = kde_operators.kde
di = data_item.DataItem.from_vals
ds = data_slice.DataSlice.from_vals
DATA_SLICE = qtypes.DATA_SLICE


QTYPES = frozenset([
    (DATA_SLICE, DATA_SLICE),
])


class StringsUpperTest(parameterized.TestCase):

  @parameterized.parameters(
      # DataItem
      (di('abcde'), di('ABCDE')),
      # DataSlice
      (ds('abcde'), ds('ABCDE')),
      (
          ds(['abc', None, '', 'ef']),
          ds(['ABC', None, '', 'EF']),
      ),
      (
          ds([['abc', None], ['', 'ef']]),
          ds([['ABC', None], ['', 'EF']]),
      ),
      (ds('你好'), ds('你好')),
      # OBJECT/ANY
      (
          ds(['abc', None, '', 'ef'], schema_constants.OBJECT),
          ds(['ABC', None, '', 'EF']),
      ),
      (
          ds(['abc', None, '', 'ef'], schema_constants.ANY),
          ds(['ABC', None, '', 'EF']),
      ),
      # Empty and unknown inputs.
      (
          ds([None, None, None], schema_constants.OBJECT),
          ds([None, None, None], schema_constants.TEXT),
      ),
      (
          ds([None, None, None]),
          ds([None, None, None], schema_constants.TEXT),
      ),
      (
          ds([None, None, None], schema_constants.ANY),
          ds([None, None, None], schema_constants.TEXT),
      ),
      (
          ds([None, None, None], schema_constants.TEXT),
          ds([None, None, None], schema_constants.TEXT),
      ),
  )
  def test_eval(self, x, expected):
    result = expr_eval.eval(kde.strings.upper(I.x), x=x)
    testing.assert_equal(result, expected)

  def test_errors(self):
    x = data_slice.DataSlice.from_vals([1, 2, 3])
    with self.assertRaisesRegex(
        ValueError,
        # TODO: Make errors Koda friendly.
        'expected texts or array of texts, got x: DENSE_ARRAY_INT32',
    ):
      expr_eval.eval(kde.strings.upper(I.x), x=x)

    x = data_slice.DataSlice.from_vals([b'abc', b'def'])
    with self.assertRaisesRegex(
        ValueError, 'expected texts or array of texts, got x: DENSE_ARRAY_BYTES'
    ):
      expr_eval.eval(kde.strings.upper(I.x), x=x)

    x = data_slice.DataSlice.from_vals(['abc', b'def'])
    with self.assertRaisesRegex(
        ValueError, 'DataSlice with mixed types is not supported'
    ):
      expr_eval.eval(kde.strings.upper(I.x), x=x)

  def test_qtype_signatures(self):
    self.assertCountEqual(
        arolla.testing.detect_qtype_signatures(
            kde.strings.upper,
            possible_qtypes=test_qtypes.DETECT_SIGNATURES_QTYPES,
        ),
        QTYPES,
    )

  def test_view(self):
    self.assertTrue(view.has_data_slice_view(kde.strings.upper(I.x)))


if __name__ == '__main__':
  absltest.main()