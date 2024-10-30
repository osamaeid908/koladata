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
from koladata.expr import expr_eval
from koladata.expr import input_container
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


I = input_container.InputContainer('I')
kde = kde_operators.kde
bag = data_bag.DataBag.empty
ds = data_slice.DataSlice.from_vals
DATA_SLICE = qtypes.DATA_SLICE


def generate_qtypes():
  for first_arg in [DATA_SLICE, arolla.UNSPECIFIED]:
    for itemid_arg_type in [DATA_SLICE, arolla.UNSPECIFIED]:
      for attrs_type in [
          arolla.make_namedtuple_qtype(),
          arolla.make_namedtuple_qtype(a=DATA_SLICE),
          arolla.make_namedtuple_qtype(a=DATA_SLICE, b=DATA_SLICE),
      ]:
        yield first_arg, itemid_arg_type, attrs_type, arolla.INT64, DATA_SLICE


QTYPES = list(generate_qtypes())


class CoreObjTest(parameterized.TestCase):

  def test_item(self):
    x = kde.core.obj(
        a=ds(3.14, schema_constants.FLOAT64),
        b=ds('abc', schema_constants.TEXT),
    ).eval()
    self.assertIsInstance(x, data_item.DataItem)
    testing.assert_equal(x.no_bag().get_schema(), schema_constants.OBJECT)
    testing.assert_allclose(
        x.a, ds(3.14, schema_constants.FLOAT64).with_bag(x.get_bag())
    )
    testing.assert_equal(
        x.a.get_schema(), schema_constants.FLOAT64.with_bag(x.get_bag())
    )
    testing.assert_equal(
        x.b.get_schema(), schema_constants.TEXT.with_bag(x.get_bag())
    )

  def test_slice(self):
    x = kde.core.obj(
        a=ds([[1, 2], [3]]),
        b=kde.core.obj(bb=ds([['a', 'b'], ['c']])),
        c=ds(b'xyz'),
    ).eval()
    testing.assert_equal(x.a, ds([[1, 2], [3]]).with_bag(x.get_bag()))
    testing.assert_equal(x.b.bb, ds([['a', 'b'], ['c']]).with_bag(x.get_bag()))
    testing.assert_equal(
        x.c, ds([[b'xyz', b'xyz'], [b'xyz']]).with_bag(x.get_bag())
    )
    testing.assert_equal(
        x.a.get_schema(), schema_constants.INT32.with_bag(x.get_bag())
    )
    testing.assert_equal(x.b.no_bag().get_schema(), schema_constants.OBJECT)
    testing.assert_equal(
        x.b.bb.get_schema(), schema_constants.TEXT.with_bag(x.get_bag())
    )
    testing.assert_equal(
        x.c.get_schema(), schema_constants.BYTES.with_bag(x.get_bag())
    )

  def test_adopt_bag(self):
    x = kde.core.obj(
        a=ds(3.14, schema_constants.FLOAT64),
        b=ds('abc', schema_constants.TEXT),
    ).eval()
    y = bag().obj(x=x)
    # y.get_bag() is merged with x.get_bag(), so access to `a` is possible.
    testing.assert_allclose(
        y.x.a, ds(3.14, schema_constants.FLOAT64).with_bag(y.get_bag())
    )
    testing.assert_equal(y.x.b, ds('abc').with_bag(y.get_bag()))
    testing.assert_equal(x.get_schema(), y.x.get_schema().with_bag(x.get_bag()))
    testing.assert_equal(y.x.a.no_bag().get_schema(), schema_constants.FLOAT64)
    testing.assert_equal(y.x.b.no_bag().get_schema(), schema_constants.TEXT)

  def test_itemid(self):
    itemid = kde.allocation.new_itemid_shaped_as._eval(ds([[1, 1], [1]]))  # pylint: disable=protected-access
    x = kde.core.obj(a=42, itemid=itemid).eval()
    testing.assert_equal(x.a.no_bag(), ds([[42, 42], [42]]))
    testing.assert_equal(x.no_bag().as_itemid(), itemid)

  def test_schema_arg(self):
    with self.assertRaisesRegex(ValueError, 'please use new'):
      kde.core.obj(a=1, b='a', schema=schema_constants.INT32).eval()

  @parameterized.parameters(
      (42, ds(42, schema_constants.OBJECT)),
      (ds([1, 2, 3]), ds([1, 2, 3], schema_constants.OBJECT)),
      (None, ds(None, schema_constants.OBJECT)),
      (
          ds(['abc', None, b'xyz']),
          ds(['abc', None, b'xyz'], schema_constants.OBJECT),
      ),
  )
  def test_converter_primitives(self, x, expected):
    testing.assert_equal(kde.core.obj(x).eval().no_bag(), expected)

  def test_converter_list(self):
    l = kde.core.obj(bag().list([1, 2, 3])).eval()
    testing.assert_equal(l.get_schema().no_bag(), schema_constants.OBJECT)
    testing.assert_equal(l[:], ds([1, 2, 3]).with_bag(l.get_bag()))

  def test_converter_empty_list(self):
    l = kde.core.obj(bag().list()).eval()
    testing.assert_equal(
        l.get_obj_schema().get_attr('__items__').no_bag(),
        schema_constants.OBJECT,
    )
    testing.assert_equal(l[:].no_bag(), ds([]))

  def test_converter_dict(self):
    d = kde.core.obj(
        bag().dict({'a': 42, 'b': ds(37, schema_constants.INT64)})
    ).eval()
    testing.assert_equal(d.get_schema().no_bag(), schema_constants.OBJECT)
    testing.assert_dicts_keys_equal(d, ds(['a', 'b']))
    testing.assert_equal(
        d[['a', 'b']],
        ds([42, 37], schema_constants.INT64).with_bag(d.get_bag()),
    )

  def test_converter_empty_dict(self):
    d = kde.core.obj(bag().dict()).eval()
    testing.assert_equal(
        d.get_obj_schema().get_attr('__keys__').no_bag(),
        schema_constants.OBJECT,
    )
    testing.assert_equal(
        d.get_obj_schema().get_attr('__values__').no_bag(),
        schema_constants.OBJECT,
    )
    testing.assert_dicts_keys_equal(d, ds([]))
    testing.assert_equal(d[ds([])].no_bag(), ds([]))

  def test_converter_entity(self):
    with self.subTest('item'):
      entity = bag().new(a=42, b='abc')
      obj = kde.core.obj(entity).eval()
      testing.assert_equal(obj.get_schema().no_bag(), schema_constants.OBJECT)
      testing.assert_equal(obj.a, ds(42).with_bag(obj.get_bag()))
      testing.assert_equal(obj.b, ds('abc').with_bag(obj.get_bag()))
    with self.subTest('slice'):
      entity = bag().new(a=ds([1, 2]), b='abc')
      obj = kde.core.obj(entity).eval()
      testing.assert_equal(obj.get_schema().no_bag(), schema_constants.OBJECT)
      testing.assert_equal(obj.a, ds([1, 2]).with_bag(obj.get_bag()))
      testing.assert_equal(obj.b, ds(['abc', 'abc']).with_bag(obj.get_bag()))

  def test_converter_object(self):
    obj = bag().obj(a=42, b='abc')
    new_obj = kde.core.obj(obj).eval()
    self.assertNotEqual(
        obj.get_bag().fingerprint, new_obj.get_bag().fingerprint
    )
    testing.assert_equivalent(new_obj, obj)

  def test_converter_into_itemid_is_not_supported(self):
    with self.assertRaisesRegex(
        ValueError, '`itemid` is not supported when converting to object'
    ):
      kde.core.obj(I.x, itemid=I.itemid).eval(x=ds(42), itemid=bag().new())

  def test_converter_on_python_objects(self):
    with self.assertRaisesRegex(
        ValueError, 'object with unsupported type: "dict"'
    ):
      kde.core.obj({'a': 42}, itemid=I.itemid)

  def test_non_determinism(self):
    res_1 = expr_eval.eval(kde.core.obj(a=I.a), a=42)
    res_2 = expr_eval.eval(kde.core.obj(a=I.a), a=42)
    self.assertNotEqual(res_1.no_bag(), res_2.no_bag())
    testing.assert_equal(res_1.a.no_bag(), res_2.a.no_bag())

    expr = kde.core.obj(a=42)
    res_1 = expr_eval.eval(expr)
    res_2 = expr_eval.eval(expr)
    self.assertNotEqual(res_1.no_bag(), res_2.no_bag())
    testing.assert_equal(res_1.a.no_bag(), res_2.a.no_bag())

  def test_qtype_signatures(self):
    arolla.testing.assert_qtype_signatures(
        kde.core.obj,
        QTYPES,
        possible_qtypes=test_qtypes.DETECT_SIGNATURES_QTYPES
        + (
            arolla.make_namedtuple_qtype(),
            arolla.make_namedtuple_qtype(a=DATA_SLICE),
            arolla.make_namedtuple_qtype(a=DATA_SLICE, b=DATA_SLICE),
        ),
    )

  def test_view(self):
    self.assertTrue(view.has_data_slice_view(kde.core.obj(I.x)))
    self.assertTrue(
        view.has_data_slice_view(kde.core.obj(I.x, itemid=I.itemid, a=I.y))
    )

  def test_alias(self):
    self.assertTrue(optools.equiv_to_op(kde.core.obj, kde.obj))

  def test_repr(self):
    self.assertEqual(
        repr(kde.core.obj(a=I.y)),
        'kde.core.obj(unspecified, itemid=unspecified, a=I.y)',
    )


if __name__ == '__main__':
  absltest.main()
