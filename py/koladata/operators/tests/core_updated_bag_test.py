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
from arolla import arolla
from koladata.expr import expr_eval
from koladata.expr import input_container
from koladata.expr import view
from koladata.operators import kde_operators
from koladata.operators import optools
from koladata.operators.tests.util import qtypes as test_qtypes
from koladata.testing import testing
from koladata.types import data_bag
from koladata.types import data_slice
from koladata.types import qtypes


bag = data_bag.DataBag.empty
I = input_container.InputContainer('I')
kde = kde_operators.kde
ds = data_slice.DataSlice.from_vals

DATA_BAG = qtypes.DATA_BAG

QTYPES = frozenset([
    (DATA_BAG,),
    (DATA_BAG, DATA_BAG),
    (DATA_BAG, DATA_BAG, DATA_BAG),
    (DATA_BAG, DATA_BAG, DATA_BAG, DATA_BAG),
    # etc.
])


class CoreUpdatedBagTest(absltest.TestCase):

  def test_updated_bag(self):
    db1 = bag()
    o1 = db1.uuobj(x=1)
    db2 = bag()
    o2 = db2.uuobj(x=1)
    o2.x = 2
    o2.y = 3
    db3 = bag()
    o3 = db3.uuobj(x=1)
    o3.x = 2
    o3.y = 4
    db3 = expr_eval.eval(
        kde.core.updated_bag(I.x, I.y, I.z), x=db1, y=db2, z=db3)
    o4 = o1.stub().with_bag(db3)
    testing.assert_equal(o4.x.no_bag(), ds(2))
    testing.assert_equal(o4.y.no_bag(), ds(4))

  def test_not_enough_args(self):
    with self.assertRaisesRegex(
        ValueError,
        'requires at least 2 arguments'
    ):
      _ = expr_eval.eval(kde.core.updated_bag())

    with self.assertRaisesRegex(
        ValueError,
        'requires at least 2 arguments'
    ):
      _ = expr_eval.eval(kde.core.updated_bag(I.x), x=bag())

  def test_qtype_signatures(self):
    arolla.testing.assert_qtype_signatures(
        kde.core.updated_bag,
        QTYPES,
        possible_qtypes=test_qtypes.DETECT_SIGNATURES_QTYPES,
        max_arity=3,
    )

  def test_view(self):
    self.assertTrue(view.has_koda_view(kde.core.updated_bag(I.x, I.y)))

  def test_alias(self):
    self.assertTrue(optools.equiv_to_op(kde.core.updated_bag, kde.updated_bag))


if __name__ == '__main__':
  absltest.main()
