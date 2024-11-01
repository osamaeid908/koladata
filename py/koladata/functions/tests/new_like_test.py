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

"""Tests for kd.new_like."""
import re

from absl.testing import absltest
from koladata.exceptions import exceptions
from koladata.functions import functions as fns
from koladata.operators import kde_operators
from koladata.testing import testing
from koladata.types import data_item
from koladata.types import data_slice
from koladata.types import dict_item
from koladata.types import list_item
from koladata.types import schema_constants


ds = data_slice.DataSlice.from_vals
kde = kde_operators.kde


class NewLikeTest(absltest.TestCase):

  def test_item(self):
    x = fns.new_like(
        ds(1),
        a=ds(3.14, schema_constants.FLOAT64),
        b=ds('abc', schema_constants.STRING),
    )
    self.assertIsInstance(x, data_item.DataItem)
    testing.assert_allclose(
        x.a, ds(3.14, schema_constants.FLOAT64).with_bag(x.get_bag())
    )
    testing.assert_equal(
        x.get_schema().a, schema_constants.FLOAT64.with_bag(x.get_bag())
    )
    testing.assert_equal(
        x.get_schema().b, schema_constants.STRING.with_bag(x.get_bag())
    )

    x = fns.new_like(ds(None), a=42)
    self.assertIsInstance(x, data_item.DataItem)
    testing.assert_equal(
        kde.has._eval(x).no_bag(), ds(None, schema_constants.MASK)
    )
    testing.assert_equal(
        x.a, ds(None, schema_constants.INT32).with_bag(x.get_bag())
    )
    testing.assert_equal(x.get_schema().a.no_bag(), schema_constants.INT32)

  def test_slice(self):
    x = fns.new_like(
        ds([[1, None], [1]]),
        a=ds([[1, 2], [3]]),
        b=fns.new(bb=ds([['a', 'b'], ['c']])),
        c=ds(b'xyz'),
    )
    testing.assert_equal(x.a, ds([[1, None], [3]]).with_bag(x.get_bag()))
    testing.assert_equal(x.b.bb, ds([['a', None], ['c']]).with_bag(x.get_bag()))
    testing.assert_equal(
        x.c, ds([[b'xyz', None], [b'xyz']]).with_bag(x.get_bag())
    )
    testing.assert_equal(
        x.get_schema().a, schema_constants.INT32.with_bag(x.get_bag())
    )
    testing.assert_equal(
        x.get_schema().b.bb, schema_constants.STRING.with_bag(x.get_bag())
    )
    testing.assert_equal(
        x.get_schema().c, schema_constants.BYTES.with_bag(x.get_bag())
    )

  def test_broadcast_attrs(self):
    x = fns.new_like(ds([1, 1]), a=42, b='xyz')
    testing.assert_equal(x.a, ds([42, 42]).with_bag(x.get_bag()))
    testing.assert_equal(x.b, ds(['xyz', 'xyz']).with_bag(x.get_bag()))

  def test_broadcast_error(self):
    with self.assertRaisesRegex(exceptions.KodaError, 'cannot be expanded'):
      fns.new_like(ds([1, 1]), a=ds([42]))

  def test_all_empty_slice(self):
    x = fns.new_like(ds([None, None]), a=42)
    testing.assert_equal(
        x.a, ds([None, None], schema_constants.INT32).with_bag(x.get_bag())
    )
    testing.assert_equal(x.get_schema().a.no_bag(), schema_constants.INT32)

  def test_adopt_bag(self):
    x = fns.new_like(ds(1))
    x.set_attr('a', 'abc', update_schema=True)
    y = fns.new_like(x, x=x)
    # y.get_bag() is merged with x.get_bag(), so access to `a` is possible.
    testing.assert_equal(y.x.a, ds('abc').with_bag(y.get_bag()))
    testing.assert_equal(x.get_schema(), y.get_schema().x.with_bag(x.get_bag()))
    testing.assert_equal(y.x.a.no_bag().get_schema(), schema_constants.STRING)

  def test_itemid(self):
    itemid = kde.allocation.new_itemid_shaped_as._eval(ds([[1, 1], [1]]))  # pylint: disable=protected-access
    x = fns.new_like(ds([[1, None], [1]]), a=42, itemid=itemid)
    testing.assert_equal(x.a.no_bag(), ds([[42, None], [42]]))
    testing.assert_equal(x.no_bag().get_itemid(), itemid & kde.has._eval(x))  # pylint: disable=protected-access

  def test_itemid_from_different_bag(self):
    itemid = fns.new(non_existent=ds([[42, 42], [42]])).get_itemid()
    assert itemid.get_bag() is not None
    # Successful.
    x = fns.new_like(ds([[1, None], [1]]), a=42, itemid=itemid)
    # ITEMID's triples are stripped in the new DataBag.
    with self.assertRaisesRegex(
        ValueError, 'attribute \'non_existent\' is missing'
    ):
      _ = x.non_existent

  def test_bag_arg(self):
    db = fns.bag()
    x = fns.new_like(ds(1), a=1, b='a', db=db)
    testing.assert_equal(db, x.get_bag())

  def test_schema_arg(self):
    schema = fns.new_schema(a=schema_constants.INT32, b=schema_constants.STRING)
    x = fns.new_like(ds([1, None]), a=42, b='xyz', schema=schema)
    self.assertEqual(dir(x), ['a', 'b'])
    testing.assert_equal(x.a, ds([42, None]).with_bag(x.get_bag()))
    testing.assert_equal(x.get_schema().a.no_bag(), schema_constants.INT32)
    testing.assert_equal(x.b, ds(['xyz', None]).with_bag(x.get_bag()))
    testing.assert_equal(x.get_schema().b.no_bag(), schema_constants.STRING)

  def test_schema_arg_implicit_casting(self):
    schema = fns.new_schema(a=schema_constants.FLOAT32)
    x = fns.new_like(ds([1, 1]), a=42, schema=schema)
    self.assertEqual(dir(x), ['a'])
    testing.assert_equal(
        x.a, ds([42, 42], schema_constants.FLOAT32).with_bag(x.get_bag())
    )
    testing.assert_equal(x.get_schema().a.no_bag(), schema_constants.FLOAT32)

  def test_schema_arg_update_schema(self):
    schema = fns.new_schema(a=schema_constants.INT32)
    x = fns.new_like(
        ds([1, 1]),
        a=42, b='xyz',
        schema=schema, update_schema=True
    )
    self.assertEqual(dir(x), ['a', 'b'])
    testing.assert_equal(x.a, ds([42, 42]).with_bag(x.get_bag()))
    testing.assert_equal(x.get_schema().a.no_bag(), schema_constants.INT32)
    testing.assert_equal(x.b, ds(['xyz', 'xyz']).with_bag(x.get_bag()))
    testing.assert_equal(x.get_schema().b.no_bag(), schema_constants.STRING)

  def test_schema_arg_update_schema_error(self):
    with self.assertRaisesRegex(TypeError, 'expected bool'):
      fns.new_like(ds(1), schema=schema_constants.ANY, update_schema=42)  # pytype: disable=wrong-arg-types

  def test_schema_arg_update_schema_error_overwriting(self):
    schema = fns.new_schema(a=schema_constants.INT32)
    x = fns.new_like(ds(1), a='xyz', schema=schema, update_schema=True)
    testing.assert_equal(x.a, ds('xyz').with_bag(x.get_bag()))

  def test_schema_arg_errors(self):
    with self.assertRaisesRegex(
        TypeError, 'expecting schema to be a DataSlice, got int'
    ):
      fns.new_like(ds(1), a=1, schema=5)
    with self.assertRaisesRegex(
        ValueError, 'schema must be SCHEMA, got: INT32'
    ):
      fns.new_like(ds(1), a=1, schema=ds([1, 2, 3]))
    with self.assertRaisesRegex(ValueError, 'schema can only be 0-rank'):
      fns.new_like(
          ds(1),
          a=1,
          schema=ds([schema_constants.INT32, schema_constants.STRING]),
      )
    with self.assertRaisesRegex(
        exceptions.KodaError, 'requires Entity schema, got INT32'
    ):
      fns.new_like(ds(1), a=1, schema=schema_constants.INT32)
    with self.assertRaisesRegex(
        exceptions.KodaError, 'requires Entity schema, got OBJECT'
    ):
      fns.new_like(ds(1), a=1, schema=schema_constants.OBJECT)

  def test_schema_error_message(self):
    schema = fns.new_schema(a=schema_constants.INT32)
    with self.assertRaisesRegex(
        exceptions.KodaError,
        re.escape(
            r"""cannot create Item(s) with the provided schema: SCHEMA(a=INT32)

The cause is: the schema for attribute 'a' is incompatible.

Expected schema for 'a': INT32
Assigned schema for 'a': TEXT

To fix this, explicitly override schema of 'a' in the original schema. For example,
schema.a = <desired_schema>"""
        ),
    ):
      fns.new_like(ds([1, 2, 3]), a=ds(['a', 'b', 'c']), schema=schema)

    db1 = fns.bag()
    _ = db1.uuobj(x=1)
    db2 = fns.bag()
    b = db2.uuobj(x=1)
    b.x = 2
    with self.assertRaisesRegex(
        exceptions.KodaError,
        r"""cannot create Item\(s\)

The cause is: conflicting values for x for [0-9a-z]{32}:0: 1 vs 2""",
    ):
      db1.new_like(ds([1, 2, 3]), y=b)

  def test_item_assignment_rhs_no_ds_args(self):
    x = fns.new_like(ds(1), x=1, lst=[1, 2, 3], dct={'a': 42})
    self.assertIsInstance(x, data_item.DataItem)
    self.assertIsInstance(x.lst, list_item.ListItem)
    self.assertIsInstance(x.dct, dict_item.DictItem)
    testing.assert_equal(x.lst[:], ds([1, 2, 3]).with_bag(x.get_bag()))
    testing.assert_dicts_keys_equal(x.dct, ds(['a']).with_bag(x.get_bag()))

  def test_item_assignment_rhs_with_ds_args(self):
    x = fns.new_like(ds(1), x=1, y=ds('a'), lst=[1, 2, 3], dct={'a': 42})
    self.assertIsInstance(x, data_item.DataItem)
    self.assertIsInstance(x.lst, list_item.ListItem)
    self.assertIsInstance(x.dct, dict_item.DictItem)
    testing.assert_equal(x.lst[:], ds([1, 2, 3]).with_bag(x.get_bag()))
    testing.assert_dicts_keys_equal(x.dct, ds(['a']).with_bag(x.get_bag()))

  def test_slice_assignment_rhs(self):
    with self.assertRaisesRegex(ValueError, 'assigning a Python list/tuple'):
      fns.new_like(ds([1, 2, 3]), lst=[1, 2, 3])
    with self.assertRaisesRegex(ValueError, 'assigning a Python dict'):
      fns.new_like(ds([1, 2, 3]), dct={'a': 42})


if __name__ == '__main__':
  absltest.main()
