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
from koladata.exceptions import exceptions
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
kde = kde_operators.kde
ds = data_slice.DataSlice.from_vals
bag = data_bag.DataBag.empty
DATA_SLICE = qtypes.DATA_SLICE
JAGGED_SHAPE = qtypes.JAGGED_SHAPE


def generate_qtypes():
  for schema_arg_type in [DATA_SLICE, arolla.UNSPECIFIED]:
    for itemid_arg_type in [DATA_SLICE, arolla.UNSPECIFIED]:
      for attrs_type in [
          arolla.make_namedtuple_qtype(),
          arolla.make_namedtuple_qtype(a=DATA_SLICE),
          arolla.make_namedtuple_qtype(a=DATA_SLICE, b=DATA_SLICE),
      ]:
        yield JAGGED_SHAPE, schema_arg_type, itemid_arg_type, DATA_SLICE, attrs_type, DATA_SLICE


QTYPES = list(generate_qtypes())


class CoreNewShapedTest(absltest.TestCase):

  def test_slice_no_attrs(self):
    shape = jagged_shape.create_shape(2, 3)
    x = kde.core.new_shaped(shape).eval()
    testing.assert_equal(x.get_shape(), shape)
    self.assertFalse(x.is_mutable())

  def test_item_no_attrs(self):
    shape = jagged_shape.create_shape()
    x = kde.core.new_shaped(shape).eval()
    self.assertIsNotNone(x.db)
    testing.assert_equal(x.get_shape(), shape)
    self.assertFalse(x.is_mutable())

  def test_with_attrs(self):
    shape = jagged_shape.create_shape(2, 3)
    x = kde.core.new_shaped(shape, x=2, a=1, b='p', c=fns.list([5, 6])).eval()
    testing.assert_equal(x.get_shape(), shape)
    testing.assert_equal(x.x.no_db(), ds([[2, 2, 2], [2, 2, 2]]))
    testing.assert_equal(x.a.no_db(), ds([[1, 1, 1], [1, 1, 1]]))
    testing.assert_equal(x.b.no_db(), ds([['p', 'p', 'p'], ['p', 'p', 'p']]))
    testing.assert_equal(
        x.c[:].no_db(),
        ds([[[5, 6], [5, 6], [5, 6]], [[5, 6], [5, 6], [5, 6]]]),
    )
    self.assertFalse(x.is_mutable())

  def test_schema_arg_simple(self):
    shape = jagged_shape.create_shape(2, 3)
    schema = fns.new_schema(a=schema_constants.INT32, b=schema_constants.TEXT)
    x = kde.core.new_shaped(shape, schema=schema).eval()
    testing.assert_equal(x.get_shape(), shape)
    testing.assert_equal(x.get_schema().a.no_db(), schema_constants.INT32)
    testing.assert_equal(x.get_schema().b.no_db(), schema_constants.TEXT)

  def test_schema_arg_deep(self):
    nested_schema = fns.new_schema(p=schema_constants.BYTES)
    schema = fns.new_schema(
        a=schema_constants.INT32,
        b=schema_constants.TEXT,
        nested=nested_schema,
    )
    x = kde.core.new_shaped(
        jagged_shape.create_shape(),
        a=42,
        b='xyz',
        nested=fns.new_shaped(
            jagged_shape.create_shape(), p=b'0123', schema=nested_schema
        ),
        schema=schema,
    ).eval()
    self.assertEqual(dir(x), ['a', 'b', 'nested'])
    testing.assert_equal(x.a, ds(42).with_db(x.db))
    testing.assert_equal(x.get_schema().a.no_db(), schema_constants.INT32)
    testing.assert_equal(x.b, ds('xyz').with_db(x.db))
    testing.assert_equal(x.get_schema().b.no_db(), schema_constants.TEXT)
    testing.assert_equal(x.nested.p, ds(b'0123').with_db(x.db))
    testing.assert_equal(
        x.nested.get_schema().p.no_db(), schema_constants.BYTES
    )

  def test_schema_arg_implicit_casting(self):
    schema = fns.new_schema(a=schema_constants.FLOAT32)
    x = kde.core.new_shaped(
        jagged_shape.create_shape([2]), a=42, schema=schema
    ).eval()
    self.assertEqual(dir(x), ['a'])
    testing.assert_equal(
        x.a, ds([42, 42], schema_constants.FLOAT32).with_db(x.db)
    )
    testing.assert_equal(x.get_schema().a.no_db(), schema_constants.FLOAT32)

  def test_schema_arg_implicit_casting_failure(self):
    schema = fns.new_schema(a=schema_constants.INT32)
    with self.assertRaisesRegex(
        exceptions.KodaError, r'schema for attribute \'a\' is incompatible'
    ):
      kde.core.new_shaped(
          jagged_shape.create_shape([2]), a='xyz', schema=schema
      ).eval()

  def test_schema_arg_update_schema(self):
    schema = fns.new_schema(a=schema_constants.INT32)
    x = kde.core.new_shaped(
        jagged_shape.create_shape([2]),
        a=42,
        b='xyz',
        schema=schema,
        update_schema=True,
    ).eval()
    self.assertEqual(dir(x), ['a', 'b'])
    testing.assert_equal(x.a, ds([42, 42]).with_db(x.db))
    testing.assert_equal(x.get_schema().a.no_db(), schema_constants.INT32)
    testing.assert_equal(x.b, ds(['xyz', 'xyz']).with_db(x.db))
    testing.assert_equal(x.get_schema().b.no_db(), schema_constants.TEXT)

  def test_schema_arg_update_schema_error(self):
    with self.assertRaisesRegex(
        ValueError, 'update_schema must be a boolean scalar'
    ):
      kde.core.new_shaped(
          jagged_shape.create_shape(),
          schema=schema_constants.ANY,
          update_schema=42,
      ).eval()

  def test_schema_arg_update_schema_overwriting(self):
    schema = fns.new_schema(a=schema_constants.INT32)
    x = kde.core.new_shaped(
        jagged_shape.create_shape(),
        a='xyz',
        schema=schema,
        update_schema=True,
    ).eval()
    testing.assert_equal(x.a, ds('xyz').with_db(x.db))

  def test_itemid(self):
    itemid = kde.allocation.new_itemid_shaped_as._eval(ds([[1, 1], [1]]))
    x = kde.core.new_shaped(itemid.get_shape(), a=42, itemid=itemid).eval()
    testing.assert_equal(x.a.no_db(), ds([[42, 42], [42]]))
    testing.assert_equal(x.no_db().as_itemid(), itemid)

  def test_itemid_from_different_db(self):
    itemid = fns.new(non_existent=ds([[42, 42], [42]])).as_itemid()
    assert itemid.db is not None
    x = kde.core.new_shaped(itemid.get_shape(), a=42, itemid=itemid).eval()
    with self.assertRaisesRegex(
        ValueError, "attribute 'non_existent' is missing"
    ):
      _ = x.non_existent

  def test_fails_without_shape(self):
    with self.assertRaisesRegex(
        TypeError, "missing required positional argument: 'shape'"
    ):
      _ = kde.core.new_shaped().eval()

  def test_fails_with_dataslice_input(self):
    with self.assertRaisesRegex(ValueError, 'expected JAGGED_SHAPE'):
      _ = kde.core.new_shaped(ds(0)).eval()

  def test_qtype_signatures(self):
    arolla.testing.assert_qtype_signatures(
        kde.core.new_shaped,
        QTYPES,
        possible_qtypes=test_qtypes.DETECT_SIGNATURES_QTYPES
        + (
            arolla.make_namedtuple_qtype(),
            arolla.make_namedtuple_qtype(a=DATA_SLICE),
            arolla.make_namedtuple_qtype(a=DATA_SLICE, b=DATA_SLICE),
        ),
    )

  def test_view(self):
    self.assertTrue(view.has_data_slice_view(kde.core.new_shaped(I.x)))
    self.assertTrue(view.has_data_slice_view(kde.core.new_shaped(I.x, a=I.y)))

  def test_alias(self):
    self.assertTrue(optools.equiv_to_op(kde.core.new_shaped, kde.new_shaped))


if __name__ == '__main__':
  absltest.main()