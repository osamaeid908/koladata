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
from koladata.operators import optools
from koladata.operators.tests.util import qtypes as test_qtypes
from koladata.types import data_bag
from koladata.types import data_slice
from koladata.types import qtypes

I = input_container.InputContainer('I')
kde = kde_operators.kde
bag = data_bag.DataBag.empty()
ds = data_slice.DataSlice.from_vals
DATA_SLICE = qtypes.DATA_SLICE


class CoreEncodeItemIdTest(parameterized.TestCase):

  def test_encode(self):
    item = expr_eval.eval(kde.core.encode_itemid(bag.new(x=1)))
    self.assertRegex(str(item), '[0-9a-zA-Z]*')

    items = expr_eval.eval(kde.core.encode_itemid(bag.new(x=ds([1, 2]))))
    self.assertRegex(str(items.L[0]), '[0-9a-zA-Z]*')
    self.assertRegex(str(items.L[1]), '[0-9a-zA-Z]*')

  def test_error(self):
    with self.assertRaisesRegex(
        exceptions.KodaError, 'only ObjectIds can be encoded, got INT32'
    ):
      kde.core.encode_itemid(ds([1, 2, 3])).eval()

  def test_qtype_signatures(self):
    self.assertCountEqual(
        arolla.testing.detect_qtype_signatures(
            kde.core.encode_itemid,
            possible_qtypes=test_qtypes.DETECT_SIGNATURES_QTYPES,
        ),
        frozenset([(DATA_SLICE, DATA_SLICE)]),
    )

  def test_view(self):
    self.assertTrue(view.has_data_slice_view(kde.core.encode_itemid(I.ds)))

  def test_alias(self):
    self.assertTrue(
        optools.equiv_to_op(kde.core.encode_itemid, kde.encode_itemid)
    )


if __name__ == '__main__':
  absltest.main()