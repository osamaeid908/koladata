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

"""Tests for allocation.new_itemid."""

from absl.testing import absltest
from arolla import arolla
from koladata.expr import expr_eval
from koladata.expr import view
from koladata.operators import kde_operators
from koladata.operators import optools
from koladata.operators.tests.util import qtypes as test_qtypes
from koladata.testing import testing
from koladata.types import data_bag
from koladata.types import data_item
from koladata.types import data_slice
from koladata.types import qtypes
from koladata.types import schema_constants

bag = data_bag.DataBag.empty
ds = data_slice.DataSlice.from_vals
kde = kde_operators.kde


# TODO: Test non-determinism when it gets implemented.
class AllocationNewItemIdTest(absltest.TestCase):

  def test_eval(self):
    itemid = expr_eval.eval(kde.allocation.new_itemid())
    self.assertIsInstance(itemid, data_item.DataItem)
    testing.assert_equal(itemid.get_schema(), schema_constants.ITEMID)
    entity = itemid.with_db(bag())
    entity = entity.with_schema(entity.db.new_schema(a=schema_constants.INT32))
    entity.a = 42
    testing.assert_equal(entity.a, ds(42).with_db(entity.db))

  def test_new_alloc_ids(self):
    itemid = expr_eval.eval(kde.allocation.new_itemid())
    testing.assert_equal(itemid, expr_eval.eval(kde.allocation.new_itemid()))
    arolla.abc.clear_caches()
    self.assertNotEqual(itemid, expr_eval.eval(kde.allocation.new_itemid()))

  def test_qtype_signatures(self):
    self.assertCountEqual(
        arolla.testing.detect_qtype_signatures(
            kde.allocation.new_itemid,
            possible_qtypes=test_qtypes.DETECT_SIGNATURES_QTYPES,
        ),
        frozenset([(qtypes.DATA_SLICE,)]),
    )

  def test_view(self):
    self.assertTrue(view.has_data_slice_view(kde.allocation.new_itemid()))

  def test_alias(self):
    self.assertTrue(
        optools.equiv_to_op(kde.allocation.new_itemid, kde.new_itemid)
    )


if __name__ == '__main__':
  absltest.main()