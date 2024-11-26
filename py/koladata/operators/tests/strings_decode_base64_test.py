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

from absl.testing import absltest
from absl.testing import parameterized
from arolla import arolla
from koladata.exceptions import exceptions
from koladata.expr import expr_eval
from koladata.expr import input_container
from koladata.expr import view
from koladata.operators import kde_operators
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
    (DATA_SLICE, arolla.UNSPECIFIED, DATA_SLICE),
    (DATA_SLICE, DATA_SLICE, DATA_SLICE),
])


class StringsDecodeBase64Test(parameterized.TestCase):

  @parameterized.parameters(
      (ds(None, schema_constants.STRING), ds(None, schema_constants.BYTES)),
      (ds(None, schema_constants.BYTES), ds(None, schema_constants.BYTES)),
      (ds(None, schema_constants.ANY), ds(None, schema_constants.BYTES)),
      (ds(None, schema_constants.OBJECT), ds(None, schema_constants.BYTES)),
      (ds(''), ds(b'')),
      (ds(b''), ds(b'')),
      (ds('Zm9v'), ds(b'foo')),
      (ds(b'Zm9v'), ds(b'foo')),
      (ds('AP/M'), ds(b'\x00\xff\xcc')),
      (ds('YWFhYQ=='), ds(b'aaaa')),
      (ds('YWFhYQ'), ds(b'aaaa')),
      (ds('YWFhYQ..'), ds(b'aaaa')),
      (ds('YWF hYQ'), ds(b'aaaa')),
      (ds('YWF\nhYQ'), ds(b'aaaa')),
      (ds(b'Zm9v', schema_constants.ANY), ds(b'foo')),
      (ds(b'Zm9v', schema_constants.OBJECT), ds(b'foo')),
      (ds([None], schema_constants.STRING), ds([None], schema_constants.BYTES)),
      (ds(['Zm9v']), ds([b'foo'])),
      (ds(['Zm9v'], schema_constants.ANY), ds([b'foo'])),
      (ds(['Zm9v', 'YmFy'], schema_constants.ANY), ds([b'foo', b'bar'])),
      # Invalid converted to missing because of on_invalid=None
      (ds('???'), ds(None, schema_constants.BYTES)),
      (ds('_'), ds(None, schema_constants.BYTES)),
      (ds('YWFhYQ='), ds(None, schema_constants.BYTES)),
  )
  def test_eval(self, x, expected):
    res = expr_eval.eval(kde.strings.decode_base64(x, on_invalid=None))
    testing.assert_equal(res, expected)

  @parameterized.parameters(
      (ds('???'),),
      (ds('_'),),
      (ds('YWFhYQ='),),
  )
  def test_eval_invalid_base64(self, x):
    with self.assertRaisesRegex(
        exceptions.KodaError,
        'kd.strings.decode_base64: invalid base64 string:',
    ):
      _ = expr_eval.eval(kde.strings.decode_base64(x))

  def test_on_invalid_none(self):
    x = ds([['Zm9v', '???'], ['???', 'YmFy', '???']])
    result = expr_eval.eval(kde.strings.decode_base64(x, on_invalid=None))
    testing.assert_equal(result, ds([[b'foo', None], [None, b'bar', None]]))

  def test_on_invalid_value(self):
    x = ds([['Zm9v', '???'], ['???', 'YmFy', '???']])
    result = expr_eval.eval(kde.strings.decode_base64(x, on_invalid=-1))
    testing.assert_equal(result, ds([[b'foo', -1], [-1, b'bar', -1]]))

  def test_on_invalid_nontrivial_broadcast(self):
    x = ds([['Zm9v', '???'], ['???', 'YmFy', '???']])
    result = expr_eval.eval(kde.strings.decode_base64(x, on_invalid=ds([1, 2])))
    testing.assert_equal(result, ds([[b'foo', 1], [2, b'bar', 2]]))

  def test_on_invalid_error(self):
    x = ds([['Zm9v', '???'], ['???', 'YmFy', '???']])
    with self.assertRaisesRegex(ValueError, 'shapes are not compatible'):
      _ = expr_eval.eval(kde.strings.decode_base64(x, on_invalid=ds([1, 2, 3])))

  def test_schema_error(self):
    with self.assertRaisesWithLiteralMatch(
        exceptions.KodaError,
        'kd.strings.decode_base64: argument `x` must be a slice of strings or'
        ' byteses, got a slice of INT32',
    ):
      _ = kde.strings.decode_base64(ds(1)).eval()

  def test_dtype_error(self):
    with self.assertRaisesWithLiteralMatch(
        exceptions.KodaError,
        'kd.strings.decode_base64: argument `x` must be a slice of strings or'
        ' byteses, got a slice of OBJECT with an item of type INT32',
    ):
      _ = kde.strings.decode_base64(ds(1, schema_constants.OBJECT)).eval()

  def test_qtype_signatures(self):
    arolla.testing.assert_qtype_signatures(
        kde.strings.decode_base64,
        QTYPES,
        possible_qtypes=test_qtypes.DETECT_SIGNATURES_QTYPES,
    )

  def test_view(self):
    self.assertTrue(view.has_koda_view(kde.strings.decode_base64(I.x)))


if __name__ == '__main__':
  absltest.main()
