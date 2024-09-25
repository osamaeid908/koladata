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

"""Tests for list_shaped."""

from absl.testing import absltest
from absl.testing import parameterized
from koladata.exceptions import exceptions
from koladata.functions import functions as fns
# Register kde ops for e.g. jagged_shape.create_shape().
from koladata.operators import kde_operators
from koladata.testing import testing
from koladata.types import data_slice
from koladata.types import jagged_shape
from koladata.types import list_item
from koladata.types import schema_constants


ds = data_slice.DataSlice.from_vals
kde = kde_operators.kde


class ListShapedTest(parameterized.TestCase):

  def test_item(self):
    shape = jagged_shape.create_shape()
    l = fns.list_shaped(shape)
    self.assertIsInstance(l, list_item.ListItem)
    testing.assert_equal(l[:], ds([]).with_db(l.db))
    l.append(1)
    testing.assert_equal(l[:], ds([1], schema_constants.OBJECT).with_db(l.db))

  def test_item_with_items(self):
    shape = jagged_shape.create_shape()
    l = fns.list_shaped(shape, [1, 2])
    self.assertIsInstance(l, list_item.ListItem)
    testing.assert_equal(l[:], ds([1, 2]).with_db(l.db))
    l.append(3)
    testing.assert_equal(l[:], ds([1, 2, 3]).with_db(l.db))

  def test_slice(self):
    shape = jagged_shape.create_shape([2], [2, 1])
    l = fns.list_shaped(shape)
    self.assertIsInstance(l, data_slice.DataSlice)
    testing.assert_equal(l[:], ds([[[], []], [[]]]).with_db(l.db))
    l.append(1)
    testing.assert_equal(
        l[:], ds([[[1], [1]], [[1]]], schema_constants.OBJECT).with_db(l.db)
    )

  def test_slice_with_scalar_items(self):
    shape = jagged_shape.create_shape([2], [2, 1])
    l = fns.list_shaped(shape, 1)
    self.assertIsInstance(l, data_slice.DataSlice)
    testing.assert_equal(l[:], ds([[[1], [1]], [[1]]]).with_db(l.db))
    l.append(2)
    testing.assert_equal(l[:], ds([[[1, 2], [1, 2]], [[1, 2]]]).with_db(l.db))

  def test_slice_with_slice_items(self):
    shape = jagged_shape.create_shape([2], [2, 1])
    l = fns.list_shaped(shape, ds([[[1, 2], [3, 4]], [[5, 6]]]))
    self.assertIsInstance(l, data_slice.DataSlice)
    testing.assert_equal(l[:], ds([[[1, 2], [3, 4]], [[5, 6]]]).with_db(l.db))
    l.append(7)
    testing.assert_equal(
        l[:], ds([[[1, 2, 7], [3, 4, 7]], [[5, 6, 7]]]).with_db(l.db)
    )

  def test_itemid(self):
    itemid = kde.allocation.new_listid_shaped_as._eval(ds([1, 1]))  # pylint: disable=protected-access
    x = fns.list_shaped(
        itemid.get_shape(), ds([['a', 'b'], ['c']]), itemid=itemid
    )
    testing.assert_equal(x[:].no_db(), ds([['a', 'b'], ['c']]))
    testing.assert_equal(x.no_db().as_itemid(), itemid)

  def test_itemid_from_different_db(self):
    triple = fns.new(non_existent=42)
    itemid = fns.list(ds([[[triple], []], [[]]]))

    # Successful.
    x = fns.list_shaped(itemid.get_shape(), itemid=itemid.as_itemid())
    # ITEMID's triples are stripped in the new DataBag.
    with self.assertRaisesRegex(
        ValueError, 'attribute \'non_existent\' is missing'
    ):
      _ = triple.with_db(x.db).non_existent

  def test_db_arg(self):
    shape = jagged_shape.create_shape([2], [2, 1])
    db = fns.bag()
    items = ds([[[1], [2]], [[3]]])
    l = fns.list_shaped(shape, items)
    with self.assertRaises(AssertionError):
      testing.assert_equal(l.db, db)

    l = fns.list_shaped(shape, items, db=db)
    testing.assert_equal(l.db, db)

  def test_wrong_shape(self):
    with self.assertRaisesRegex(
        TypeError, 'expecting shape to be a JaggedShape, got int'
    ):
      fns.list_shaped(57)  # pytype: disable=wrong-arg-types

  def test_imcompatible_shape(self):
    shape = jagged_shape.create_shape([2], [2, 1])
    with self.assertRaisesRegex(ValueError, 'cannot be expanded'):
      fns.list_shaped(shape, [1, 2, 3])

  @parameterized.parameters(
      # No schema, no list items provided.
      (None, None, None, schema_constants.OBJECT),
      # Schema, but no list items provided.
      (None, schema_constants.OBJECT, None, schema_constants.OBJECT),
      (None, schema_constants.INT64, None, schema_constants.INT64),
      (None, schema_constants.ANY, None, schema_constants.ANY),
      # Deduce schema from list items.
      ([[1, 2], [3]], None, None, schema_constants.INT32),
      ([[1, 'foo'], [3]], None, None, schema_constants.OBJECT),
      (
          ds([[1, 'foo'], [3]], dtype=schema_constants.ANY),
          None,
          None,
          schema_constants.ANY,
      ),
      # Both schema and list items provided.
      ([[1, 2], [3]], schema_constants.INT64, None, schema_constants.INT64),
      ([[1, 2], [3]], schema_constants.ANY, None, schema_constants.ANY),
      ([[1, 2], [3]], schema_constants.OBJECT, None, schema_constants.OBJECT),
      (
          None,
          None,
          fns.list_schema(item_schema=schema_constants.INT64),
          schema_constants.INT64,
      ),
      (
          [[1, 2], [3]],
          None,
          fns.list_schema(item_schema=schema_constants.INT64),
          schema_constants.INT64,
      ),
  )
  def test_schema(self, items, item_schema, schema, expected_item_schema):
    shape = jagged_shape.create_shape([2], [2, 1])
    testing.assert_equal(
        fns.list_shaped(
            shape, items=items, item_schema=item_schema, schema=schema,
        )
        .get_schema()
        .get_attr('__items__').with_db(None),
        expected_item_schema,
    )

  def test_schema_arg_error(self):
    shape = jagged_shape.create_shape([2], [2, 1])
    list_schema = fns.list_schema(item_schema=schema_constants.INT64)
    with self.assertRaisesRegex(
        ValueError, 'either a list schema or item schema, but not both'
    ):
      fns.list_shaped(
          shape,
          item_schema=schema_constants.INT64,
          schema=list_schema,
      )

  def test_wrong_arg_types(self):
    shape = ds([[0, 0], [0]]).get_shape()
    with self.assertRaisesRegex(
        TypeError, 'expecting item_schema to be a DataSlice, got int'
    ):
      fns.list_shaped(shape, item_schema=42)
    with self.assertRaisesRegex(
        TypeError, 'expecting schema to be a DataSlice, got int'
    ):
      fns.list_shaped(shape, schema=42)

  def test_schema_errors(self):
    with self.assertRaisesRegex(
        exceptions.KodaError,
        r"""the schema for List item is incompatible.

Expected schema for List item: BYTES
Assigned schema for List item: INT32""",
    ):
      fns.list_shaped(
          jagged_shape.create_shape([2], [2, 1]),
          items=[[1, 2], [3]],
          item_schema=schema_constants.BYTES,
      )


if __name__ == '__main__':
  absltest.main()