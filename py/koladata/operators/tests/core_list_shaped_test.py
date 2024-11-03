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

"""Tests for kde.core.list_shaped operator."""

import itertools

from absl.testing import absltest
from absl.testing import parameterized
from arolla import arolla
from koladata.exceptions import exceptions
from koladata.expr import expr_eval
from koladata.expr import input_container
from koladata.expr import view
from koladata.functions import functions as fns
from koladata.operators import kde_operators
from koladata.operators import optools
from koladata.operators.tests.util import qtypes as test_qtypes
from koladata.testing import testing
from koladata.types import data_bag
from koladata.types import data_slice
from koladata.types import jagged_shape
from koladata.types import qtypes
from koladata.types import schema_constants


I = input_container.InputContainer('I')
ds = data_slice.DataSlice.from_vals
kde = kde_operators.kde
bag = data_bag.DataBag.empty

QTYPE_SIGNATURES = list(
    (qtypes.JAGGED_SHAPE, *args, arolla.INT64, qtypes.DATA_SLICE)
    for args in itertools.product(
        [qtypes.DATA_SLICE, arolla.UNSPECIFIED], repeat=4
    )
)


class ListShapedTest(parameterized.TestCase):

  @parameterized.parameters(
      ([jagged_shape.create_shape()], dict()),
      ([jagged_shape.create_shape([0])], dict()),
      ([jagged_shape.create_shape([1])], dict(items=ds([2]))),
      ([jagged_shape.create_shape([2], [2, 1])], dict()),
      (
          [jagged_shape.create_shape([2])],
          dict(item_schema=schema_constants.OBJECT),
      ),
      (
          [jagged_shape.create_shape([2])],
          dict(item_schema=schema_constants.INT64),
      ),
      (
          [jagged_shape.create_shape([2])],
          dict(item_schema=schema_constants.ANY),
      ),
      ([jagged_shape.create_shape([2])], dict(items=ds([[1, 2], [3]]))),
      (
          [jagged_shape.create_shape([2])],
          dict(items=ds([[1, 'foo'], [3]])),
      ),
      (
          [jagged_shape.create_shape([2])],
          dict(items=ds([[1, 'foo'], [3]], schema=schema_constants.ANY)),
      ),
      (
          [jagged_shape.create_shape([2])],
          dict(
              items=ds([[1, 'foo'], [3]], schema=schema_constants.OBJECT),
              item_schema=schema_constants.ANY,
          ),
      ),
      (
          [jagged_shape.create_shape([2])],
          dict(
              schema=bag().list_schema(item_schema=schema_constants.INT64),
          ),
      ),
      (
          [jagged_shape.create_shape([2])],
          dict(
              items=ds([[1, 2], [3]]),
              schema=bag().list_schema(item_schema=schema_constants.INT64),
          ),
      ),
      (
          [jagged_shape.create_shape([2])],
          dict(
              itemid=bag()
              .list_shaped(jagged_shape.create_shape([2]))
              .get_itemid()
          ),
      ),
  )
  def test_value(self, args, kwargs):
    actual = expr_eval.eval(kde.core.list_shaped(*args, **kwargs))
    expected = bag().list_shaped(*args, **kwargs)
    testing.assert_equal(actual[:].no_bag(), expected[:].no_bag())

  def test_itemid(self):
    itemid = kde.allocation.new_listid_shaped_as._eval(ds([1, 1]))  # pylint: disable=protected-access
    x = expr_eval.eval(
        kde.core.list_shaped(
            jagged_shape.create_shape([2]),
            items=ds([['a', 'b'], ['c']]),
            itemid=itemid,
        )
    )
    testing.assert_equal(x[:].no_bag(), ds([['a', 'b'], ['c']]))
    testing.assert_equal(x.no_bag().get_itemid(), itemid & kde.has._eval(x))  # pylint: disable=protected-access

  def test_db_is_immutable(self):
    lst = expr_eval.eval(kde.core.list_shaped(jagged_shape.create_shape([2])))
    self.assertFalse(lst.is_mutable())

  def test_wrong_shape_and_mask_from(self):
    with self.assertRaisesRegex(
        ValueError,
        'expected JAGGED_SHAPE, got shape: DATA_BAG',
    ):
      expr_eval.eval(kde.core.list_shaped(fns.bag()))

  def test_incompatible_shape(self):
    with self.assertRaisesRegex(ValueError, 'cannot be expanded'):
      expr_eval.eval(
          kde.core.list_shaped(
              jagged_shape.create_shape([2]), items=ds([1, 2, 3])
          )
      )

  def test_schema_arg_error(self):
    list_schema = fns.list_schema(item_schema=schema_constants.INT64)
    with self.assertRaisesRegex(
        ValueError, 'either a list schema or item schema, but not both'
    ):
      expr_eval.eval(kde.core.list_shaped(
          jagged_shape.create_shape([2], [2, 1]),
          item_schema=schema_constants.INT64,
          schema=list_schema,
      ))

  def test_wrong_arg_types(self):
    shape = jagged_shape.create_shape([2], [2, 1])
    with self.assertRaisesRegex(
        ValueError, "schema's schema must be SCHEMA, got: INT32"
    ):
      expr_eval.eval(kde.core.list_shaped(shape, item_schema=42))
    with self.assertRaisesRegex(
        ValueError, "schema's schema must be SCHEMA, got: INT32"
    ):
      expr_eval.eval(kde.core.list_shaped(shape, schema=42))

  def test_schema_errors(self):
    with self.assertRaisesRegex(
        exceptions.KodaError,
        r"""the schema for List item is incompatible.

Expected schema for List item: BYTES
Assigned schema for List item: INT32""",
    ):
      expr_eval.eval(kde.core.list_shaped(
          jagged_shape.create_shape([2]),
          items=ds([[1, 2], [3]]),
          item_schema=schema_constants.BYTES,
      ))

  def test_non_determinism(self):
    shape = jagged_shape.create_shape([2])
    items = ds([[1, 2], [3]])
    res_1 = expr_eval.eval(kde.core.list_shaped(shape, items=items))
    res_2 = expr_eval.eval(kde.core.list_shaped(shape, items=items))
    self.assertNotEqual(res_1.db.fingerprint, res_2.db.fingerprint)
    testing.assert_equal(res_1[:].no_bag(), res_2[:].no_bag())

    expr = kde.core.list_shaped(shape, items=items)
    res_1 = expr_eval.eval(expr)
    res_2 = expr_eval.eval(expr)
    self.assertNotEqual(res_1.db.fingerprint, res_2.db.fingerprint)
    testing.assert_equal(res_1[:].no_bag(), res_2[:].no_bag())

  def test_qtype_signatures(self):
    arolla.testing.assert_qtype_signatures(
        kde.core.list_shaped,
        QTYPE_SIGNATURES,
        possible_qtypes=test_qtypes.DETECT_SIGNATURES_QTYPES,
    )

  def test_view(self):
    self.assertTrue(view.has_data_slice_view(kde.core.list_shaped(I.x)))

  def test_alias(self):
    self.assertTrue(optools.equiv_to_op(kde.core.list_shaped, kde.list_shaped))

  def test_repr(self):
    self.assertEqual(
        repr(kde.core.list_shaped(I.x, schema=I.y)),
        'kde.core.list_shaped(I.x, unspecified, item_schema=unspecified,'
        ' schema=I.y, itemid=unspecified)',
    )

if __name__ == '__main__':
  absltest.main()