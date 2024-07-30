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

"""Tests for data_slice."""

import gc
import re
import sys

from absl.testing import absltest
from absl.testing import parameterized
from arolla import arolla
from koladata.exceptions import exceptions
from koladata.operators import kde_operators
from koladata.testing import testing
from koladata.types import data_bag
from koladata.types import data_item as _  # pylint: disable=unused-import
from koladata.types import data_slice
from koladata.types import dict_item as _  # pylint: disable=unused-import
from koladata.types import jagged_shape
from koladata.types import list_item as _  # pylint: disable=unused-import
from koladata.types import qtypes
from koladata.types import schema_constants

kde = kde_operators.kde
bag = data_bag.DataBag.empty
ds = data_slice.DataSlice.from_vals


class DataSliceMethodsTest(absltest.TestCase):

  def test_add_method(self):
    self.assertFalse(hasattr(data_slice.DataSlice, 'foo'))

    @data_slice.DataSlice.add_method('foo')
    def foo(self):
      """Converts DataSlice to Python list."""
      return self.internal_as_py()

    self.assertTrue(hasattr(data_slice.DataSlice, 'foo'))

    x = ds([1, 2, 3])
    self.assertEqual(x.foo(), [1, 2, 3])

    with self.assertRaisesRegex(TypeError, 'method name must be a string'):
      data_slice.DataSlice.internal_register_reserved_class_method_name(b'foo')

    with self.assertRaisesRegex(
        AttributeError, r'object attribute \'foo\' is read-only'
    ):
      x.foo = 42

  def test_add_method_to_subclass(self):

    class SubDataSlice(data_slice.DataSlice):
      pass

    self.assertFalse(hasattr(data_slice.DataSlice, 'bar'))
    self.assertFalse(hasattr(SubDataSlice, 'bar'))

    @SubDataSlice.add_method('bar')
    def bar(self):
      del self
      pass

    self.assertFalse(hasattr(data_slice.DataSlice, 'bar'))
    self.assertTrue(hasattr(SubDataSlice, 'bar'))

  def test_subclass(self):

    x = bag().obj()
    x.some_method = 42
    testing.assert_equal(x.some_method, ds(42).with_db(x.db))

    @data_slice.register_reserved_class_method_names
    class SubDataSlice(data_slice.DataSlice):

      def some_method(self):
        pass

    self.assertFalse(hasattr(data_slice.DataSlice, 'some_method'))
    self.assertTrue(hasattr(SubDataSlice, 'some_method'))

    with self.assertRaisesRegex(
        AttributeError, r'has no attribute \'some_method\''
    ):
      _ = x.some_method
    with self.assertRaisesRegex(
        AttributeError, r'has no attribute \'some_method\''
    ):
      x.some_method = 42

  def test_subclass_error(self):

    with self.assertRaises(AssertionError):

      @data_slice.register_reserved_class_method_names
      class NonDataSliceSubType:
        pass

      del NonDataSliceSubType


class DataSliceTest(parameterized.TestCase):

  def test_ref_count(self):
    gc.collect()
    diff_count = 10
    base_count = sys.getrefcount(data_slice.DataSlice)
    slices = []
    for _ in range(diff_count):
      slices.append(ds([1]))  # Adding DataSlice(s)

    self.assertEqual(
        sys.getrefcount(data_slice.DataSlice), base_count + diff_count
    )

    # NOTE: ds() invokes `PyDataSlice_Type()` C Python function multiple times
    # and this verifies there are no leaking references.
    del slices
    gc.collect()
    self.assertEqual(sys.getrefcount(data_slice.DataSlice), base_count)

    # NOTE: with_schema() invokes `PyDataSlice_Type()` C Python function and
    # this verifies there are no leaking references.
    ds(1).as_any()
    self.assertEqual(sys.getrefcount(data_slice.DataSlice), base_count)

    items = []
    for _ in range(diff_count):
      items.append(ds(1))  # Adding DataItem(s)
    self.assertEqual(sys.getrefcount(data_slice.DataSlice), base_count)

    res = ds([1]) + ds([2])
    self.assertEqual(sys.getrefcount(data_slice.DataSlice), base_count + 1)
    del res

    _ = ds([1, 2, 3]).S[0]
    self.assertEqual(sys.getrefcount(data_slice.DataSlice), base_count)

  def test_qvalue(self):
    self.assertTrue(issubclass(data_slice.DataSlice, arolla.QValue))
    x = ds([1, 2, 3])
    self.assertIsInstance(x, arolla.QValue)

  def test_fingerprint(self):
    x1 = ds(arolla.dense_array_int64([None, None]))
    x2 = ds([None, None], schema_constants.INT64)
    self.assertEqual(x1.fingerprint, x2.fingerprint)
    x3 = x2.with_schema(schema_constants.INT32)
    self.assertNotEqual(x1.fingerprint, x3.fingerprint)
    x4 = x2.with_schema(schema_constants.TEXT)
    self.assertNotEqual(x1.fingerprint, x4.fingerprint)

    db = bag()
    self.assertNotEqual(x1.with_db(db).fingerprint, x2.fingerprint)
    self.assertEqual(x1.with_db(db).fingerprint, x2.with_db(db).fingerprint)

    shape = jagged_shape.create_shape([2], [1, 1])
    self.assertNotEqual(x1.reshape(shape).fingerprint, x2.fingerprint)
    self.assertEqual(
        x1.reshape(shape).fingerprint, x2.reshape(shape).fingerprint
    )

    self.assertEqual(ds([42, 'abc']).fingerprint, ds([42, 'abc']).fingerprint)
    self.assertNotEqual(
        ds([42, 'abc']).fingerprint,
        ds([42, b'abc']).fingerprint,
    )

    self.assertNotEqual(ds([1]).fingerprint, ds(1).fingerprint)

  @parameterized.named_parameters(
      (
          'data_item',
          ds(12),
          'DataItem(12, schema: INT32)',
      ),
      (
          'int32',
          ds([1, 2]),
          'DataSlice([1, 2], schema: INT32, shape: JaggedShape(2))',
      ),
      (
          'int64',
          ds([1, 2], schema_constants.INT64),
          'DataSlice([1, 2], schema: INT64, shape: JaggedShape(2))',
      ),
      (
          'float32',
          ds([1.0, 1.5]),
          'DataSlice([1.0, 1.5], schema: FLOAT32, shape: JaggedShape(2))',
      ),
      (
          'float64',
          ds([1.0, 1.5], schema_constants.FLOAT64),
          'DataSlice([1.0, 1.5], schema: FLOAT64, shape: JaggedShape(2))',
      ),
      (
          'boolean',
          ds([True, False]),
          'DataSlice([True, False], schema: BOOLEAN, shape: JaggedShape(2))',
      ),
      (
          'mask',
          ds([arolla.unit(), arolla.optional_unit(None)]),
          'DataSlice([present, None], schema: MASK, shape: JaggedShape(2))',
      ),
      (
          'text',
          ds(['a', 'b']),
          "DataSlice(['a', 'b'], schema: TEXT, shape: JaggedShape(2))",
      ),
      (
          'bytes',
          ds([b'a', b'b']),
          "DataSlice([b'a', b'b'], schema: BYTES, shape: JaggedShape(2))",
      ),
      (
          'int32_with_any',
          ds([1, 2]).as_any(),
          'DataSlice([1, 2], schema: ANY, shape: JaggedShape(2))',
      ),
      (
          'int32_with_object',
          ds([1, 2]).with_schema(schema_constants.OBJECT),
          'DataSlice([1, 2], schema: OBJECT, shape: JaggedShape(2))',
      ),
      (
          'mixed_data',
          ds([1, 'abc', True, 1.0, arolla.int64(1)]),
          (
              "DataSlice([1, 'abc', True, 1.0, 1], schema: OBJECT, shape:"
              ' JaggedShape(5))'
          ),
      ),
      (
          'int32_with_none',
          ds([1, None]),
          'DataSlice([1, None], schema: INT32, shape: JaggedShape(2))',
      ),
      (
          'empty',
          ds([], schema_constants.INT64),
          'DataSlice([], schema: INT64, shape: JaggedShape(0))',
      ),
      (
          'empty_int64_internal',
          ds(arolla.dense_array_int64([])),
          'DataSlice([], schema: INT64, shape: JaggedShape(0))',
      ),
      (
          'multidim',
          ds([[[1], [2]], [[3], [4], [5]]]),
          (
              'DataSlice([[[1], [2]], [[3], [4], [5]]], schema: INT32, '
              'shape: JaggedShape(2, [2, 3], 1))'
          ),
      ),
  )
  def test_repr_no_db(self, x, expected_repr):
    self.assertEqual(repr(x), expected_repr)

  def test_repr_with_db(self):
    db = bag()
    x = ds([1, 2]).with_db(db)
    bag_id = '$' + str(db.fingerprint)[-4:]
    self.assertEqual(
        repr(x),
        'DataSlice([1, 2], schema: INT32, shape: JaggedShape(2), '
        f'bag_id: {bag_id})',
    )

  # NOTE: DataSlice has custom __eq__ which works pointwise and returns another
  # DataSlice. So multi-dim DataSlices cannot be used as Python dict keys.
  def test_non_hashable(self):
    with self.assertRaisesRegex(TypeError, 'unhashable type'):
      hash(ds([1, 2, 3]))

  def test_db(self):
    x = ds([1, 2, 3])
    self.assertIsNone(x.db)

    db = bag()
    x = x.with_db(db)
    self.assertIsNotNone(x.db)

    self.assertIsNone(x.with_db(None).db)
    self.assertIsNone(x.no_db().db)

    x = db.new_shaped(jagged_shape.create_shape([1]))
    self.assertIsNotNone(x.db)
    # NOTE: At the moment x.db is not db. If this is needed, we could store the
    # db PyObject* reference in PyDataSlice object. The underlying DataBag that
    # PyObject points to, is the same.
    self.assertIsNot(x.db, db)

    with self.assertRaisesRegex(
        TypeError, 'expecting db to be a DataBag, got list'
    ):
      x.with_db([1, 2, 3])

    with self.assertRaisesRegex(
        TypeError, 'expecting db to be a DataBag, got DenseArray'
    ):
      x.with_db(arolla.dense_array([1, 2, 3]))

  def test_fork_db(self):
    x = ds([1, 2, 3])

    with self.assertRaisesRegex(
        ValueError, 'fork_db expects the DataSlice to have a DataBag attached'
    ):
      x.fork_db()

    db = data_bag.DataBag.empty()
    x = x.with_db(db)

    x1 = x.fork_db()
    self.assertIsNot(x, x1)
    self.assertIsNot(x1.db, x.db)
    self.assertIsNot(x1.db, db)

  def test_reserved_ipython_method_names(self):
    db = bag()
    x = db.new(getdoc=1, trait_names=2, normal=3)
    self.assertEqual(x.normal, 3)
    with self.assertRaises(AttributeError):
      _ = x.getdoc
    with self.assertRaises(AttributeError):
      _ = x.trait_names

  def test_dir(self):
    db = bag()
    fb = bag()
    x = db.new(a=1, b='abc')
    self.assertEqual(dir(x), ['a', 'b'])
    self.assertEqual(dir(ds([x])), ['a', 'b'])
    x.with_db(fb).set_attr('c', 42, update_schema=True)
    self.assertEqual(dir(x.with_db(db).with_fallback(fb)), ['a', 'b', 'c'])
    self.assertEqual(
        dir(ds([x]).with_db(db).with_fallback(fb)),
        ['a', 'b', 'c'],
    )

  def test_to_py(self):
    x = ds([[1, 2], [3], [4, 5]])
    self.assertEqual(x.to_py(), [[1, 2], [3], [4, 5]])

  def test_assignment_rhs_koda_iterables(self):
    db = bag()
    x = db.obj()
    # Text
    x.a = 'abc'
    self.assertEqual(x.a, 'abc')
    # Bytes
    x.b = b'abc'
    self.assertEqual(x.b, b'abc')
    x.a = []
    testing.assert_equal(
        x.a.get_schema().get_attr('__items__'),
        schema_constants.OBJECT.with_db(db),
    )
    x.a = ()
    testing.assert_equal(
        x.a.get_schema().get_attr('__items__'),
        schema_constants.OBJECT.with_db(db),
    )
    # Other iterables are not supported in boxing code.
    with self.assertRaisesRegex(ValueError, 'object with unsupported type'):
      x.a = set()
    with self.assertRaisesRegex(ValueError, 'object with unsupported type'):
      x.a = iter([1, 2, 3])

  def test_set_get_attr(self):
    db = bag()
    x = db.new(abc=ds([42]))
    x.get_schema().xyz = schema_constants.INT64
    x.xyz = ds([12], schema_constants.INT64)
    testing.assert_equal(x.abc, ds([42]).with_db(db))
    testing.assert_equal(x.abc.get_schema(), schema_constants.INT32.with_db(db))
    testing.assert_equal(x.xyz, ds([12], schema_constants.INT64).with_db(db))
    testing.assert_equal(x.xyz.get_schema(), schema_constants.INT64.with_db(db))

  def test_set_get_attr_methods(self):
    db = bag()

    with self.subTest('entity'):
      x = db.new(abc=ds([42], schema_constants.INT64))
      testing.assert_equal(
          x.get_attr('abc'), ds([42], schema_constants.INT64).with_db(db)
      )
      testing.assert_equal(
          x.get_attr('abc').get_schema(), schema_constants.INT64.with_db(db)
      )
      # Missing
      with self.assertRaisesRegex(ValueError, r'attribute \'xyz\' is missing'):
        x.get_attr('xyz')
      testing.assert_equal(x.get_attr('xyz', None), ds([None]).with_db(db))
      testing.assert_equal(x.get_attr('xyz', b'b'), ds([b'b']).with_db(db))

      with self.assertRaisesRegex(
          ValueError, r'The attribute \'xyz\' is missing on the schema'
      ):
        x.set_attr('xyz', ds([12]), update_schema=False)

      x.set_attr('xyz', ds([12]), update_schema=True)
      testing.assert_equal(x.get_attr('xyz'), ds([12]).with_db(db))
      testing.assert_equal(
          x.get_attr('xyz').get_schema(), schema_constants.INT32.with_db(db)
      )

    with self.subTest('object'):
      x = db.obj(abc=ds([42], schema_constants.INT64))
      testing.assert_equal(
          x.get_attr('abc'), ds([42], schema_constants.INT64).with_db(db)
      )
      testing.assert_equal(
          x.get_attr('abc').get_schema(), schema_constants.INT64.with_db(db)
      )

      for attr, val, update_schema, res_schema in [
          ('xyz', ds([b'12']), True, schema_constants.BYTES),
          ('pqr', ds(['123']), False, schema_constants.TEXT),
      ]:
        x.set_attr(attr, val, update_schema=update_schema)
        testing.assert_equal(x.get_attr(attr), val.with_db(db))
        testing.assert_equal(
            x.get_attr(attr).get_schema(), res_schema.with_db(db)
        )
        testing.assert_equal(
            x.get_attr('__schema__').get_attr(attr),
            ds([res_schema]).with_db(db),
        )

    with self.subTest('objects with explicit schema'):
      x = db.obj(abc=ds([42, 12]))
      e_schema = db.new(abc=ds(42, schema_constants.INT64)).get_schema()
      x.set_attr('__schema__', e_schema)
      # TODO: The following assertion returns different
      # fingerprints. Most likely due to data stored as INT32, even if schema is
      # INT64.
      # testing.assert_equal(
      #     x.get_attr('abc'),
      #     ds([42, 12], schema_constants.INT64).with_db(db)
      # )
      self.assertEqual(x.get_attr('abc').internal_as_py(), [42, 12])
      testing.assert_equal(
          x.get_attr('abc').get_schema(), schema_constants.INT64.with_db(db)
      )

      x.set_attr(
          'abc',
          # Casting INT32 -> INT64 is allowed and done automatically.
          ds([1, 2], schema_constants.INT32),
      )
      testing.assert_equal(
          x.get_attr('abc'), ds([1, 2], schema_constants.INT64).with_db(db)
      )
      testing.assert_equal(
          x.get_attr('abc').get_schema(), schema_constants.INT64.with_db(db)
      )

      with self.assertRaisesRegex(
          ValueError, r'The schema for attribute \'abc\' is incompatible'
      ):
        x.set_attr('abc', ds([b'x', b'y']), update_schema=False)
      # Overwrite with overwriting schema.
      x.set_attr('abc', ds([b'x', b'y']), update_schema=True)
      testing.assert_equal(x.get_attr('abc'), ds([b'x', b'y']).with_db(db))
      testing.assert_equal(
          x.get_attr('abc').get_schema(), schema_constants.BYTES.with_db(db)
      )
      testing.assert_equal(
          x.get_attr('__schema__').get_attr('abc'),
          ds([schema_constants.BYTES, schema_constants.BYTES]).with_db(db),
      )

    with self.subTest('errors'):
      x = db.new(abc=ds([42], schema_constants.INT64))
      with self.assertRaises(TypeError):
        x.set_attr(b'invalid_attr', 1)
      with self.assertRaises(ValueError):
        x.set_attr('invalid__val', ValueError)
      with self.assertRaisesRegex(TypeError, 'expected bool'):
        x.set_attr('invalid__update_schema_type', 1, update_schema=42)
      with self.assertRaisesRegex(
          TypeError, 'accepts 2 to 3 positional arguments'
      ):
        x.set_attr('invalid__update_schema_type', 1, False, 42)

  def test_set_get_attr_empty_attr_name(self):
    db = bag()
    x = db.new()
    setattr(x.get_schema(), '', schema_constants.INT32)
    setattr(x, '', 1)
    testing.assert_equal(getattr(x, ''), ds(1).with_db(db))

  def test_set_attr_auto_broadcasting(self):
    db = bag()
    x = db.new_shaped(jagged_shape.create_shape([3]))
    x.get_schema().xyz = schema_constants.INT32
    x.xyz = ds(12)
    testing.assert_equal(x.xyz, ds([12, 12, 12]).with_db(db))

    x_abc = ds([12, 12])
    with self.assertRaisesRegex(
        ValueError,
        re.escape(
            f'DataSlice with shape={x_abc.get_shape()} cannot be expanded to'
            f' shape={x.get_shape()}'
        ),
    ):
      x.abc = x_abc

  def test_set_get_attr_empty_entity(self):
    x = bag().new(a=1) & ds(None)
    testing.assert_equal(x.a, ds(None, schema_constants.INT32).with_db(x.db))
    x = bag().new_shaped(jagged_shape.create_shape([2]), a=1) & ds(None)
    testing.assert_equal(
        x.a, ds([None, None], schema_constants.INT32).with_db(x.db)
    )

  def test_set_get_attr_empty_object(self):
    x = bag().obj(a=1) & ds(None)
    testing.assert_equal(x.a, ds(None, schema_constants.OBJECT).with_db(x.db))
    x = bag().obj_shaped(jagged_shape.create_shape([2]), a=1) & ds(None)
    testing.assert_equal(
        x.a, ds([None, None], schema_constants.OBJECT).with_db(x.db)
    )

  def test_set_get_attr_object_missing_schema_attr(self):
    obj = bag().obj(a=1)
    with self.assertRaisesRegex(
        exceptions.KodaError, 'object schema is missing for the DataItem'
    ):
      _ = obj.with_db(bag()).a
    with self.assertRaisesRegex(
        exceptions.KodaError, 'object schema is missing for the DataItem'
    ):
      obj.with_db(bag()).a = 1
    with self.assertRaisesRegex(
        exceptions.KodaError, r'object schema is missing for the DataItem'
    ):
      del obj.with_db(bag()).a

  def test_set_get_attr_slice_of_objects_missing_schema_attr(self):
    obj_1 = bag().obj(a=1)
    obj_2 = bag().new(a=1).with_schema(schema_constants.OBJECT)
    obj = ds([obj_1, obj_2])
    with self.assertRaisesRegex(
        exceptions.KodaError, re.escape('object schema(s) are missing')
    ):
      _ = obj.a
    with self.assertRaisesRegex(
        exceptions.KodaError, re.escape('object schema(s) are missing')
    ):
      obj.a = 1
    with self.assertRaisesRegex(
        exceptions.KodaError, re.escape('object schema(s) are missing')
    ):
      del obj.a

  def test_set_get_attr_object_wrong_schema_attr(self):
    obj = bag().obj(a=1)
    obj.set_attr('__schema__', schema_constants.INT32)
    with self.assertRaisesRegex(
        ValueError, 'cannot get or set attributes on schema constants: INT32'
    ):
      _ = obj.a
    with self.assertRaisesRegex(
        ValueError, 'cannot get or set attributes on schema constants: INT32'
    ):
      obj.a = 1
    with self.assertRaisesRegex(
        ValueError,
        r'objects must have ObjectId\(s\) as __schema__ attribute, got INT32'
    ):
      del obj.a

  def test_set_attr_merging(self):
    db1 = bag()
    db2 = bag()

    obj1 = db1.obj(a=1)
    obj2 = db1.obj(a=2)
    obj3 = db1.obj(a=3)
    root = db2.obj(b=ds([4, 5, 6]))
    root.c = ds([obj1, obj2, obj3])

    testing.assert_equal(root.c.a, ds([1, 2, 3]).with_db(root.db))

  def test_set_get_attr_on_qvalue_properties(self):
    x = bag().obj()
    # qtype.
    x.set_attr('qtype', 42)
    testing.assert_equal(x.get_attr('qtype'), ds(42).with_db(x.db))
    self.assertEqual(x.qtype, qtypes.DATA_SLICE)
    with self.assertRaisesRegex(
        AttributeError, r'attribute \'qtype\'.*is not writable'
    ):
      x.qtype = 42
    # fingerprint.
    x.set_attr('fingerprint', 42)
    testing.assert_equal(x.get_attr('fingerprint'), ds(42).with_db(x.db))
    with self.assertRaisesRegex(
        AttributeError, r'attribute \'fingerprint\'.*is not writable'
    ):
      x.fingerprint = 42
    # DataSlice's specific property `db`.
    x.set_attr('db', 42)
    testing.assert_equal(x.get_attr('db'), ds(42).with_db(x.db))
    testing.assert_equal(x.db, x.db)
    with self.assertRaisesRegex(
        AttributeError, r'attribute \'db\'.*is not writable'
    ):
      x.db = 42

  def test_getattr_errors(self):
    x = ds([1, 2, 3])
    with self.assertRaisesRegex(
        ValueError,
        'cannot fetch attributes without a DataBag: abc',
    ):
      _ = x.abc
    with self.assertRaisesRegex(TypeError, r'attribute name must be string'):
      getattr(x, 12345)  # pytype: disable=wrong-arg-types

  def test_set_attr_none(self):
    with self.subTest('entity'):
      e = bag().new(x=42)
      e.x = None
      testing.assert_equal(
          e.x.get_schema(), schema_constants.INT32.with_db(e.db)
      )

      db = bag()
      e = db.new(x=db.new())
      e.x = None
      testing.assert_equal(e.x.get_schema(), e.get_schema().x)

    with self.subTest('object'):
      o = bag().obj(x=42)
      o.x = None
      testing.assert_equal(
          o.x.get_schema(), schema_constants.NONE.with_db(o.db)
      )

    with self.subTest('incompatible schema'):
      with self.assertRaisesRegex(
          ValueError, r'The schema for attribute \'x\' is incompatible'
      ):
        db = bag()
        db.new(x=db.new()).x = ds(None, schema_constants.OBJECT)

    with self.subTest('schema'):
      with self.assertRaisesRegex(
          ValueError,
          'only schemas can be assigned as attributes of schemas, got: None',
      ):
        bag().new().get_schema().x = None

  def test_setattr_assignment_rhs_scalar(self):
    x = bag().obj(a=1)
    x.b = 4
    testing.assert_equal(x.b, ds(4).with_db(x.db))

  def test_setattr_assignment_rhs_auto_packing_list(self):
    x = bag().obj(a=1)
    x.b = [1, 2, 3]
    testing.assert_equal(x.b[:], ds([1, 2, 3]).with_db(x.db))
    testing.assert_equal(
        x.b.get_schema().get_attr('__items__'),
        schema_constants.INT32.with_db(x.db),
    )

  def test_setattr_assignment_rhs_auto_packing_dicts(self):
    x = bag().obj(a=1)
    x.b = {'a': {42: 3.14}, 'b': {37: 2.0}}
    testing.assert_dicts_keys_equal(x.b, ds(['a', 'b']))
    testing.assert_allclose(
        x.b[ds(['a', 'b', 'a'])][42], ds([3.14, None, 3.14]).with_db(x.db)
    )
    testing.assert_allclose(x.b['b'][37], ds(2.0).with_db(x.db))
    self.assertEqual(
        x.b.get_schema().get_attr('__keys__'), schema_constants.TEXT
    )
    self.assertEqual(
        x.b.get_schema().get_attr('__values__').get_attr('__keys__'),
        schema_constants.INT32,
    )
    self.assertEqual(
        x.b.get_schema().get_attr('__values__').get_attr('__values__'),
        schema_constants.FLOAT32,
    )

  def test_setattr_assignment_rhs_error(self):
    x = bag().obj(a=ds([1, 2, 3]))
    with self.assertRaisesRegex(
        ValueError, 'only supported for Koda List DataItem'
    ):
      x.b = [4, 5, 6]
    with self.assertRaisesRegex(ValueError, 'multi-dim DataSlice'):
      x.b = [1, 2, ds([3, 4])]
    with self.assertRaisesRegex(
        ValueError, 'only supported for Koda Dict DataItem'
    ):
      x.b = {'abc': 42}

  def test_setattr_assignment_rhs_dict_error(self):
    x = bag().obj()
    with self.assertRaisesRegex(ValueError, 'unsupported type: "Obj"'):

      class Obj:
        pass

      x.b = {'a': Obj()}

    with self.assertRaisesRegex(ValueError, 'multi-dim DataSlice'):
      x.b = {'a': ds([1, 2, 3])}

    with self.assertRaisesRegex(ValueError, 'multi-dim DataSlice'):
      x.b = {'a': {42: ds([1, 2, 3])}}

  def test_set_multiple_attrs(self):
    x = bag().new(a=1, b='a')
    x.set_attrs(a=2, b='abc')
    testing.assert_equal(x.a, ds(2).with_db(x.db))
    testing.assert_equal(x.b, ds('abc').with_db(x.db))

    with self.assertRaisesRegex(
        ValueError, r'attribute \'c\' is missing on the schema'
    ):
      x.set_attrs(a=2, b='abc', c=15)

    with self.assertRaisesRegex(
        ValueError, r'schema for attribute \'b\' is incompatible'
    ):
      x.set_attrs(a=2, b=b'abc')

    x.set_attrs(a=2, b=b'abc', update_schema=True)
    testing.assert_equal(x.a, ds(2).with_db(x.db))
    testing.assert_equal(x.b, ds(b'abc').with_db(x.db))

  def test_set_multiple_attrs_with_merging(self):
    o = bag().obj(a=1)
    b = bag().new(x='abc', y=1234)
    o.set_attrs(b=b, d={'a': 42}, l=[1, 2, 3])

    testing.assert_equal(o.a, ds(1).with_db(o.db))
    # Merged DataBag from another object / entity.
    testing.assert_equal(o.b.x, ds('abc').with_db(o.db))
    testing.assert_equal(o.b.y, ds(1234).with_db(o.db))
    # Merged DataBag from creating a DataBag during boxing of complex Python
    # values.
    testing.assert_dicts_equal(o.d, bag().dict({'a': 42}))
    testing.assert_equal(o.l[:], ds([1, 2, 3]).with_db(o.db))

  def test_set_multiple_attrs_wrong_update_schema_type(self):
    o = bag().obj()
    with self.assertRaisesRegex(
        TypeError, 'expected bool for update_schema, got int'
    ):
      o.set_attrs(update_schema=42)

  def test_del_attr(self):
    db = bag()

    with self.subTest('entity'):
      e = db.new(a=1, b=2)
      del e.a
      testing.assert_equal(e.a, ds(None, schema_constants.INT32).with_db(db))
      testing.assert_equal(e.a.get_schema(), schema_constants.INT32.with_db(db))
      del e.get_schema().b
      with self.assertRaisesRegex(ValueError, "the attribute 'b' is missing"):
        _ = e.b
      with self.assertRaisesRegex(ValueError, "the attribute 'c' is missing"):
        del e.get_schema().c
      with self.assertRaisesRegex(ValueError, "the attribute 'c' is missing"):
        del e.c

    with self.subTest('object'):
      o = db.obj(a=1, b=2)
      del o.a
      with self.assertRaisesRegex(ValueError, "the attribute 'a' is missing"):
        _ = o.a
      del o.get_attr('__schema__').b
      with self.assertRaisesRegex(ValueError, "the attribute 'b' is missing"):
        del o.b
      with self.assertRaisesRegex(ValueError, "the attribute 'c' is missing"):
        del o.c

  def test_as_arolla_value(self):
    x = ds([1, 2, 3], schema_constants.FLOAT32)
    arolla.testing.assert_qvalue_allclose(
        x.as_arolla_value(), arolla.dense_array([1.0, 2, 3], arolla.FLOAT32)
    )
    x = ds([1, 'abc', 3.14])
    with self.assertRaisesRegex(
        ValueError,
        'only DataSlices with primitive values of the same type can be '
        'converted to Arolla value, got: MIXED',
    ):
      x.as_arolla_value()

  def test_as_dense_array(self):
    x = ds([1, 2, 3], schema_constants.FLOAT32)
    arolla.testing.assert_qvalue_allclose(
        x.as_dense_array(), arolla.dense_array([1.0, 2, 3], arolla.FLOAT32)
    )
    x = ds([1, 'abc', 3.14])
    with self.assertRaisesRegex(
        ValueError,
        'only DataSlices with primitive values of the same type can be '
        'converted to Arolla value, got: MIXED',
    ):
      x.as_dense_array()

  @parameterized.parameters(
      (ds([1, 2, 3]), jagged_shape.create_shape([3])),
      (ds([[1, 2], [3]]), jagged_shape.create_shape([2], [2, 1])),
  )
  def test_get_shape(self, x, expected_shape):
    testing.assert_equal(x.get_shape(), expected_shape)

  @parameterized.parameters(
      (ds(1), jagged_shape.create_shape([1]), ds([1])),
      (ds(1), jagged_shape.create_shape([1], [1], [1]), ds([[[1]]])),
      (ds([1]), jagged_shape.create_shape(), ds(1)),
      (ds([[[1]]]), jagged_shape.create_shape([1]), ds([1])),
      (ds([[[1]]]), jagged_shape.create_shape(), ds(1)),
      (ds([[1, 2], [3]]), jagged_shape.create_shape([3]), ds([1, 2, 3])),
      (
          ds([1, 2, 3]),
          jagged_shape.create_shape([2], [2, 1]),
          ds([[1, 2], [3]]),
      ),
  )
  def test_reshape(self, x, shape, expected_output):
    new_x = x.reshape(shape)
    testing.assert_equal(new_x.get_shape(), shape)
    testing.assert_equal(new_x, expected_output)

  def test_reshape_incompatible_shape_exception(self):
    x = ds([1, 2, 3])
    with self.assertRaisesRegex(
        ValueError,
        'shape size must be compatible with number of items: shape_size=2 !='
        ' items_size=3',
    ):
      x.reshape(jagged_shape.create_shape([2]))

  @parameterized.parameters(1, arolla.int32(1))
  def test_reshape_non_shape(self, non_shape):
    x = ds([1, 2, 3])
    with self.assertRaisesRegex(TypeError, '`shape` must be a JaggedShape'):
      x.reshape(non_shape)

  @parameterized.parameters(
      (ds(1), ds([1])),
      (ds([[1, 2], [3, 4]]), ds([1, 2, 3, 4])),
      (ds([[[1], [2]], [[3], [4]]]), 1, ds([[1, 2], [3, 4]])),
      (ds([[[1], [2]], [[3], [4]]]), -2, ds([[1, 2], [3, 4]])),
      (ds([[[1, 2], [3]], [[4, 5]]]), 0, 2, ds([[1, 2], [3], [4, 5]])),
  )
  def test_flatten(self, *inputs_and_expected):
    args, expected = inputs_and_expected[:-1], inputs_and_expected[-1]
    flattened = args[0].flatten(*args[1:])
    testing.assert_equal(flattened, expected)

  @parameterized.parameters(
      (ds(1), 2, ds([1, 1])),
      (ds([1, 2]), 2, ds([[1, 1], [2, 2]])),
      (ds([[1, 2], [3]]), ds([2, 3]), ds([[[1, 1], [2, 2]], [[3, 3, 3]]])),
      (ds([[1, 2], [3]]), ds([[0, 1], [2]]), ds([[[], [2]], [[3, 3]]])),
  )
  def test_add_dim_and_repeat(self, x, sizes, expected):
    testing.assert_equal(x.add_dim(sizes), expected)

  @parameterized.parameters(
      (
          ds([1, 2, 3]),
          ds([arolla.missing(), arolla.present(), arolla.missing()]),
          ds([2]),
      ),
      (
          ds([[1], [2], [3]]),
          ds([[arolla.missing()], [arolla.present()], [arolla.missing()]]),
          ds([[], [2], []]),
      ),
      (
          ds([[1], [None], [3]]),
          ds([[arolla.present()], [arolla.present()], [arolla.present()]]),
          ds([[1], [None], [3]]),
      ),
      (
          ds(['a', 1, None, 1.5]),
          ds([
              arolla.missing(),
              arolla.missing(),
              arolla.missing(),
              arolla.present(),
          ]),
          ds([1.5], schema_constants.OBJECT),
      ),
      # Test case for kd.present.
      (ds(1), ds(arolla.present()), ds(1)),
      # Test case for kd.missing.
      (ds(1), ds(arolla.missing()), ds(None, schema_constants.INT32)),
  )
  def test_select(self, x, filter_input, expected_output):
    testing.assert_equal(x.select(filter_input), expected_output)

  def test_select_filter_error(self):
    x = ds([1, 2, 3])
    with self.assertRaisesRegex(
        ValueError,
        'the schema of the filter DataSlice should only be Any, Object or Mask',
    ):
      x.select(ds([1, 2, 3]))

    with self.subTest('Shape mismatch'):
      with self.assertRaisesRegex(
          ValueError,
          r'DataSlice with shape=JaggedShape\(4\) cannot be expanded',
      ):
        x = ds([[1, 2], [None, None], [7, 8, 9]])
        y = ds([arolla.present(), arolla.present(), None, arolla.present()])
        x.select(y)

  @parameterized.parameters(
      (
          ds([[1, 2, None, 4], [None, None], [7, 8, 9]]),
          ds([[1, 2, 4], [], [7, 8, 9]]),
      ),
      (ds(1), ds(1)),
      (ds(arolla.missing()), ds(None, schema_constants.MASK)),
  )
  def test_select_present(self, x, expected_output):
    testing.assert_equal(x.select_present(), expected_output)

  @parameterized.parameters(
      # ndim=0
      (ds(1), ds([1, 2, 3]), 0, ds([1, 1, 1])),
      (ds(1), ds([[1, 2], [3]]), 0, ds([[1, 1], [1]])),
      (ds([1, 2]), ds([[1, 2], [3]]), 0, ds([[1, 1], [2]])),
      # ndim=1
      (ds([1, 2]), ds([1, 2, 3]), 1, ds([[1, 2], [1, 2], [1, 2]])),
  )
  def test_expand_to(self, source, target, ndim, expected_output):
    testing.assert_equal(source.expand_to(target, ndim), expected_output)
    if ndim == 0:
      testing.assert_equal(source.expand_to(target), expected_output)

  @parameterized.parameters(
      ([1, 2, 3], None, schema_constants.INT32),
      (['a', 'b'], None, schema_constants.TEXT),
      ([b'a', b'b', b'c'], None, schema_constants.BYTES),
      (['a', b'b', 34], None, schema_constants.OBJECT),
      ([1, 2, 3], schema_constants.INT64, schema_constants.INT64),
      ([1, 2, 3], schema_constants.FLOAT64, schema_constants.FLOAT64),
  )
  def test_get_schema(self, inputs, qtype, expected_schema):
    x = ds(inputs, qtype)
    testing.assert_equal(x.get_schema(), expected_schema)
    testing.assert_equal(x.get_schema().get_schema(), schema_constants.SCHEMA)

  def test_with_schema(self):
    db = bag()
    x = db.new(x=ds([1, 2, 3]), y='abc')
    testing.assert_equal(x.get_schema().x, schema_constants.INT32.with_db(db))
    testing.assert_equal(x.get_schema().y, schema_constants.TEXT.with_db(db))

    with self.assertRaisesRegex(
        TypeError, 'expecting schema to be a DataSlice, got data_bag.DataBag'
    ):
      x.with_schema(db)

    with self.assertRaisesRegex(ValueError, "schema's schema must be SCHEMA"):
      x.with_schema(x)

    schema = db.new(x=1, y='abc').get_schema()
    testing.assert_equal(x.with_schema(schema).get_schema(), schema)

    db_2 = bag()
    schema = db_2.new(x=1, y='abc').get_schema()
    with self.assertRaisesRegex(
        ValueError, 'with_schema does not accept schemas with different DataBag'
    ):
      x.with_schema(schema)

    non_schema = db.new().with_schema(schema_constants.SCHEMA)
    with self.assertRaisesRegex(
        ValueError, 'schema must contain either a DType or valid schema ItemId'
    ):
      x.with_schema(non_schema)

    with self.assertRaisesRegex(
        ValueError, 'a non-schema item cannot be present in a schema DataSlice'
    ):
      ds(1).with_schema(schema_constants.SCHEMA)

    # NOTE: Works without deep schema verification.
    ds([1, 'abc']).with_schema(schema_constants.SCHEMA)

  def test_set_schema(self):
    db = bag()
    x = db.new(x=ds([1, 2, 3]))

    with self.assertRaisesRegex(
        TypeError, 'expecting schema to be a DataSlice, got data_bag.DataBag'
    ):
      x.set_schema(db)

    with self.assertRaisesRegex(ValueError, "schema's schema must be SCHEMA"):
      x.set_schema(x)

    schema = db.new(x=1, y='abc').get_schema()
    testing.assert_equal(x.set_schema(schema).get_schema(), schema)

    db_2 = bag()
    schema = db_2.new(x=1, y='abc').get_schema()
    res_schema = x.set_schema(schema).get_schema()
    testing.assert_equal(res_schema, schema.with_db(db))
    testing.assert_equal(res_schema.y, schema_constants.TEXT.with_db(db))

    non_schema = db.new().set_schema(schema_constants.SCHEMA)
    with self.assertRaisesRegex(
        ValueError, 'schema must contain either a DType or valid schema ItemId'
    ):
      x.set_schema(non_schema)

    with self.assertRaisesRegex(
        ValueError,
        'cannot set an Entity schema on a DataSlice without a DataBag.',
    ):
      ds(1).set_schema(schema)

    with self.assertRaisesRegex(
        ValueError, 'a non-schema item cannot be present in a schema DataSlice'
    ):
      ds(1).with_db(db).set_schema(schema_constants.SCHEMA)

    # NOTE: Works without deep schema verification.
    ds([1, 'abc']).with_db(db).set_schema(schema_constants.SCHEMA)

  def test_dict_slice(self):
    db = bag()
    single_dict = db.dict()
    single_dict[1] = 7
    single_dict['abc'] = 3.14
    many_dicts = db.dict_shaped(jagged_shape.create_shape([3]))
    many_dicts['self'] = many_dicts
    keys345 = ds([3, 4, 5], schema_constants.INT32)
    values678 = ds([6, 7, 8], schema_constants.INT32)
    many_dicts[keys345] = values678

    with self.assertRaisesRegex(ValueError, 'cannot be expanded'):
      many_dicts[ds([['a', 'b'], ['c']])] = 42

    testing.assert_equal(single_dict.get_shape(), jagged_shape.create_shape())
    testing.assert_equal(many_dicts.get_shape(), jagged_shape.create_shape([3]))

    testing.assert_dicts_keys_equal(
        single_dict, ds(['abc', 1], schema_constants.OBJECT)
    )
    testing.assert_dicts_keys_equal(
        many_dicts,
        ds([[3, 'self'], [4, 'self'], [5, 'self']], schema_constants.OBJECT),
    )

    testing.assert_equal(
        single_dict[1], ds(7, schema_constants.OBJECT).with_db(db)
    )
    testing.assert_equal(
        single_dict[2], ds(None, schema_constants.OBJECT).with_db(db)
    )
    testing.assert_allclose(
        single_dict['abc'],
        ds(3.14, schema_constants.OBJECT).with_db(db),
    )

    testing.assert_equal(
        many_dicts[keys345],
        values678.with_schema(schema_constants.OBJECT).with_db(db),
    )
    testing.assert_equal(
        many_dicts['self'], many_dicts.with_schema(schema_constants.OBJECT)
    )

    del many_dicts[4]
    del single_dict['abc']

    testing.assert_dicts_keys_equal(
        single_dict, ds([1], schema_constants.OBJECT)
    )
    testing.assert_dicts_keys_equal(
        many_dicts,
        ds([[3, 'self'], ['self'], [5, 'self']], schema_constants.OBJECT),
    )

    single_dict[keys345] = values678
    testing.assert_dicts_keys_equal(
        single_dict, ds([1, 3, 4, 5], schema_constants.OBJECT)
    )
    testing.assert_equal(
        single_dict[keys345],
        values678.with_schema(schema_constants.OBJECT).with_db(db),
    )

    keys = ds([[1, 2], [3, 4], [5, 6]], schema_constants.INT32)
    many_dicts[keys] = 7
    testing.assert_equal(
        many_dicts[keys],
        ds([[7, 7], [7, 7], [7, 7]], schema_constants.OBJECT).with_db(db),
    )

    single_dict.clear()
    many_dicts.clear()
    testing.assert_dicts_keys_equal(
        single_dict, ds([], schema_constants.OBJECT)
    )
    testing.assert_dicts_keys_equal(
        many_dicts, ds([[], [], []], schema_constants.OBJECT)
    )

  def test_dict_objects_del_key_values(self):
    d1 = bag().dict({'a': 42, 'b': 37}).embed_schema()
    d2 = bag().dict({'a': 53, 'c': 12}).with_schema(schema_constants.OBJECT)
    d = ds([d1, d2])

    with self.assertRaisesRegex(
        exceptions.KodaError, re.escape('object schema(s) are missing')
    ):
      _ = d['a']

    with self.assertRaisesRegex(
        exceptions.KodaError, re.escape('object schema(s) are missing')
    ):
      d['a'] = 101

    with self.assertRaisesRegex(
        exceptions.KodaError, re.escape('object schema(s) are missing')
    ):
      del d['a']

  def test_dict_ops_errors(self):
    db = bag()
    non_dicts = db.new_shaped(jagged_shape.create_shape([3]))
    with self.assertRaisesRegex(ValueError, 'object with unsupported type'):
      non_dicts[set()] = 'b'
    with self.assertRaisesRegex(ValueError, 'object with unsupported type'):
      _ = non_dicts[set()]
    with self.assertRaisesRegex(ValueError, 'object with unsupported type'):
      non_dicts['a'] = ValueError
    # TODO: Better error message.
    with self.assertRaisesRegex(
        ValueError, "The attribute '__keys__' is missing on the schema."
    ):
      non_dicts['a'] = 'b'
    # TODO: Better error message.
    with self.assertRaisesRegex(
        ValueError, "The attribute '__keys__' is missing on the schema."
    ):
      _ = non_dicts['a']
    with self.assertRaisesRegex(
        ValueError,
        'trying to assign a slice with 1 dimension',
    ):
      db.dict()[1] = ds([1, 2, 3])

  def test_list_slice(self):
    db = bag()
    indices210 = ds([2, 1, 0])

    single_list = db.list()
    many_lists = db.list_shaped(jagged_shape.create_shape([3]))

    with self.assertRaisesRegex(
        ValueError,
        'trying to assign a slice with 1 dimension',
    ):
      single_list[1] = ds([1, 2, 3])

    with self.assertRaisesRegex(
        ValueError,
        'slice with 1 dimensions, while 2 dimensions are required',
    ):
      many_lists[:] = ds([1, 2, 3])

    with self.assertRaisesRegex(ValueError, 'cannot be expanded'):
      many_lists[:] = ds([[1, 2, 3], [4, 5, 6]])

    single_list[:] = ds([1, 2, 3])
    many_lists[ds(None)] = ds([1, 2, 3])
    testing.assert_equal(many_lists[:], ds([[], [], []]).with_db(db))
    many_lists[:] = ds([[1, 2, 3], [4, 5, 6], [7, 8, 9]])

    single_list[1] = 'x'
    many_lists[indices210] = ds(['a', 'b', 'c'])

    testing.assert_equal(
        single_list[-1], ds(3, schema_constants.OBJECT).with_db(db)
    )
    testing.assert_equal(single_list[indices210], ds([3, 'x', 1]).with_db(db))
    testing.assert_equal(many_lists[-1], ds(['a', 6, 9]).with_db(db))
    testing.assert_equal(
        many_lists[indices210],
        ds(['a', 'b', 'c'], schema_constants.OBJECT).with_db(db),
    )

    testing.assert_equal(single_list[:], ds([1, 'x', 3]).with_db(db))
    testing.assert_equal(single_list[1:], ds(['x', 3]).with_db(db))
    testing.assert_equal(
        many_lists[:], ds([[1, 2, 'a'], [4, 'b', 6], ['c', 8, 9]]).with_db(db)
    )
    testing.assert_equal(
        many_lists[:-1], ds([[1, 2], [4, 'b'], ['c', 8]]).with_db(db)
    )

    single_list.append(ds([5, 7]))
    many_lists.append(ds([10, 20, 30]))
    testing.assert_equal(single_list[:], ds([1, 'x', 3, 5, 7]).with_db(db))
    testing.assert_equal(
        many_lists[:],
        ds([[1, 2, 'a', 10], [4, 'b', 6, 20], ['c', 8, 9, 30]]).with_db(db),
    )

    single_list.clear()
    many_lists.clear()
    testing.assert_equal(single_list[:], ds([]).with_db(db))
    testing.assert_equal(many_lists[:], ds([[], [], []]).with_db(db))

    lst = db.list([db.obj(a=1), db.obj(a=2)])
    testing.assert_equal(lst[:].a, ds([1, 2]).with_db(db))

    with self.assertRaisesRegex(
        ValueError, 'only supported for Koda List DataItem'
    ):
      many_lists.append([4, 5, 6])
    with self.assertRaisesRegex(
        ValueError, 'only supported for Koda List DataItem'
    ):
      many_lists[:] = [[1, 2], [3], [4, 5]]

  def test_list_assign_none(self):
    db = bag()
    single_list = db.list(item_schema=schema_constants.INT32)
    many_lists = db.list_shaped(jagged_shape.create_shape([3]))

    single_list[:] = ds([1, 2, 3])
    single_list[1] = None
    testing.assert_equal(single_list[:], ds([1, None, 3]).with_db(db))

    with self.assertRaisesRegex(
        ValueError,
        'slice with 0 dimensions, while 1 dimensions are required',
    ):
      single_list[1:] = None

    many_lists[:] = ds([[1, 2, 3], [4, 5, 6], [7, 8, 9]])
    many_lists[1] = None
    many_lists[[0, 0, 2]] = ds(['a', 'b', None])
    testing.assert_equal(
        many_lists[:],
        ds([['a', None, 3], ['b', None, 6], [7, None, None]]).with_db(db),
    )

    with self.assertRaisesRegex(
        ValueError,
        'slice with 0 dimensions, while 2 dimensions are required',
    ):
      many_lists[1:] = None

  def test_del_list_items(self):
    db = bag()

    single_list = db.list(item_schema=schema_constants.INT32)
    many_lists = db.list_shaped(
        jagged_shape.create_shape([3]), item_schema=schema_constants.INT32
    )

    single_list[:] = ds([1, 2, 3])
    del single_list[1]
    del single_list[-2]
    testing.assert_equal(single_list[:], ds([3]).with_db(db))

    many_lists[:] = ds([[1, 2, 3], [4, 5, 6], [7, 8, 9]])
    del many_lists[[-2, -1, 0]]
    testing.assert_equal(
        many_lists[:], ds([[1, 3], [4, 5], [8, 9]]).with_db(db)
    )

    del many_lists[-1]
    testing.assert_equal(many_lists[:], ds([[1], [4], [8]]).with_db(db))

    single_list[:] = ds([1, 2, 3, 4])
    del single_list[1:3]
    testing.assert_equal(single_list[:], ds([1, 4]).with_db(db))
    del single_list[-1]
    testing.assert_equal(single_list[:], ds([1]).with_db(db))

    many_lists.internal_as_py()[1].append(5)
    del many_lists[-1:]
    testing.assert_equal(many_lists[:], ds([[], [4], []]).with_db(db))

    many_lists[:] = ds([[1, 2, 3], [4, 5, 6], [7, 8, 9]])
    del many_lists[[2, 1, -1]]
    testing.assert_equal(
        many_lists[:], ds([[1, 2], [4, 6], [7, 8]]).with_db(db)
    )

    del many_lists[-2]
    testing.assert_equal(many_lists[:], ds([[2], [6], [8]]).with_db(db))

    many_lists[:] = ds([[1, 2, 3], [4], [5, 6]])
    del many_lists[ds([[None, None], [None], [None]])]
    testing.assert_equal(
        many_lists[:], ds([[1, 2, 3], [4], [5, 6]]).with_db(db)
    )

    many_lists = db.list_shaped(jagged_shape.create_shape([3]))
    many_lists[:] = ds([[1, 2, 3, 'a'], [4, 5, 6, 'b'], [7, 8, 9, 'c']])
    del many_lists[2]
    testing.assert_equal(
        many_lists[:], ds([[1, 2, 'a'], [4, 5, 'b'], [7, 8, 'c']]).with_db(db)
    )
    del many_lists[2:]
    testing.assert_equal(
        many_lists[:],
        ds([[1, 2], [4, 5], [7, 8]], schema_constants.OBJECT).with_db(db),
    )

  def test_list_objects_del_items(self):
    l1 = bag().list([1, 2, 3]).embed_schema()
    l2 = bag().list([4, 5]).with_schema(schema_constants.OBJECT)
    l = ds([l1, l2])

    with self.assertRaisesRegex(
        exceptions.KodaError, re.escape('object schema(s) are missing')
    ):
      _ = l[0]
    with self.assertRaisesRegex(
        exceptions.KodaError, re.escape('object schema(s) are missing')
    ):
      _ = l[0:2]

    with self.assertRaisesRegex(
        exceptions.KodaError, re.escape('object schema(s) are missing')
    ):
      l[0] = 42
    with self.assertRaisesRegex(
        exceptions.KodaError, re.escape('object schema(s) are missing')
    ):
      l[0:2] = ds([[42], [12, 15]])

    with self.assertRaisesRegex(
        exceptions.KodaError, re.escape('object schema(s) are missing')
    ):
      del l[0]
    with self.assertRaisesRegex(
        exceptions.KodaError, re.escape('object schema(s) are missing')
    ):
      del l[0:2]

  def test_list_size(self):
    db = bag()
    l = db.list([[1, 2, 3], [4, 5]])
    testing.assert_equal(l.list_size(), ds(2, schema_constants.INT64))
    testing.assert_equal(l[:].list_size(), ds([3, 2], schema_constants.INT64))

  def test_empty_subscript_method_slice(self):
    db = bag()
    testing.assert_equal(
        ds(None).as_any().with_db(db)[:],
        ds([]).as_any().with_db(db)
    )
    testing.assert_equal(
        ds([None, None]).as_any().with_db(db)[:],
        ds([[], []]).as_any().with_db(db)
    )

    ds(None).with_db(db).as_any()[:] = ds([42])
    (db.list() & ds(None))[:] = ds([42])

    with self.assertRaisesRegex(
        ValueError, r'attribute \'__items__\' is missing'
    ):
      _ = (bag().dict() & ds(None))[:]
    with self.assertRaisesRegex(ValueError, 'list expected'):
      _ = bag().dict().as_any()[:]
    with self.assertRaisesRegex(ValueError, 'lists expected'):
      _ = bag().dict_shaped(jagged_shape.create_shape([3])).as_any()[:]

  def test_empty_subscript_method_int(self):
    db = bag()
    testing.assert_equal(
        ds(None).as_any().with_db(db)[0], ds(None).as_any().with_db(db)
    )
    testing.assert_equal(
        ds([None, None]).as_any().with_db(db)[0],
        ds([None, None]).as_any().with_db(db)
    )
    testing.assert_equal(
        (db.obj(db.dict()) & ds(None))[0],
        ds(None, schema_constants.OBJECT).with_db(db)
    )

    ds(None).with_db(db).as_any()[0] = 42
    (db.list() & ds(None))[0] = 42
    (db.dict() & ds(None))['abc'] = 42

  def test_empty_entity_subscript(self):
    db = bag()
    testing.assert_equal(
        (db.list([1, 2, 3]) & ds(None))[0],
        ds(None, schema_constants.INT32).with_db(db)
    )
    with self.assertRaisesRegex(
        ValueError, 'unsupported implicit cast from TEXT to INT64'
    ):
      _ = (db.list() & ds(None))['abc']
    with self.assertRaisesRegex(
        ValueError, 'unsupported implicit cast from TEXT to INT64'
    ):
      (db.list() & ds(None))['abc'] = 42

    testing.assert_equal(
        (db.dict({'a': 42}) & ds(None))['a'],
        ds(None, schema_constants.INT32).with_db(db)
    )
    with self.assertRaisesRegex(
        ValueError, 'schema for Dict Keys is incompatible'
    ):
      _ = (db.dict({'a': 42}) & ds(None))[42]

  def test_magic_methods(self):
    x = ds([1, 2, 3])
    y = ds([4, 5, 6])
    z = ds([1, 2, None])
    mask = ds([arolla.present(), None, arolla.present()])
    with self.subTest('add'):
      testing.assert_equal(x + y, ds([5, 7, 9]))
      # With auto-boxing
      testing.assert_equal(x + 4, ds([5, 6, 7]))
      # __radd__ with auto-boxing
      testing.assert_equal(4 + x, ds([5, 6, 7]))
    with self.subTest('sub'):
      testing.assert_equal(x - y, ds([-3, -3, -3]))
      # With auto-boxing
      testing.assert_equal(x - 4, ds([-3, -2, -1]))
      # __radd__ with auto-boxing
      testing.assert_equal(4 - x, ds([3, 2, 1]))
    with self.subTest('and'):
      testing.assert_equal(x & mask, ds([1, None, 3]))
      # only __rand__ with auto-boxing
      testing.assert_equal(1 & mask, ds([1, None, 1]))
    with self.subTest('eq'):
      testing.assert_equal(
          x == z, ds([arolla.present(), arolla.present(), None])
      )
      # With auto-boxing
      testing.assert_equal(x == 2, ds([None, arolla.present(), None]))
      testing.assert_equal(  # pytype: disable=wrong-arg-types
          2 == x, ds([None, arolla.present(), None])
      )
    with self.subTest('ne'):
      testing.assert_equal(
          x != z, ds([None, None, None], schema_constants.MASK)
      )
      # With auto-boxing
      testing.assert_equal(
          x != 2, ds([arolla.present(), None, arolla.present()])
      )
      testing.assert_equal(  # pytype: disable=wrong-arg-types
          2 != x, ds([arolla.present(), None, arolla.present()])
      )
    with self.subTest('gt'):
      testing.assert_equal(
          y > z, ds([arolla.present(), arolla.present(), None])
      )
      # With auto-boxing
      testing.assert_equal(
          x > 1, ds([None, arolla.present(), arolla.present()])
      )
      testing.assert_equal(  # pytype: disable=wrong-arg-types
          2 > x, ds([arolla.present(), None, None])
      )
    with self.subTest('ge'):
      testing.assert_equal(
          y >= z, ds([arolla.present(), arolla.present(), None])
      )
      # With auto-boxing
      testing.assert_equal(
          x >= 2, ds([None, arolla.present(), arolla.present()])
      )
      testing.assert_equal(  # pytype: disable=wrong-arg-types
          1 >= x, ds([arolla.present(), None, None])
      )
    with self.subTest('lt'):
      testing.assert_equal(y < z, ds([None, None, None], schema_constants.MASK))
      # With auto-boxing
      testing.assert_equal(x < 2, ds([arolla.present(), None, None]))
      testing.assert_equal(  # pytype: disable=wrong-arg-types
          2 < x, ds([None, None, arolla.present()])
      )
    with self.subTest('le'):
      testing.assert_equal(
          y <= z, ds([None, None, None], schema_constants.MASK)
      )
      # With auto-boxing
      testing.assert_equal(
          x <= 2, ds([arolla.present(), arolla.present(), None])
      )
      testing.assert_equal(  # pytype: disable=wrong-arg-types
          2 <= x, ds([None, arolla.present(), arolla.present()])
      )
    with self.subTest('or'):
      testing.assert_equal((x & mask) | y, ds([1, 5, 3]))
      # With auto-boxing
      testing.assert_equal((x & mask) | 4, ds([1, 4, 3]))
      # __ror__ with auto-boxing
      testing.assert_equal(None | y, ds([4, 5, 6]))
    with self.subTest('not'):
      testing.assert_equal(~mask, ds([None, arolla.present(), None]))

  def test_embed_schema_entity(self):
    db = bag()
    x = db.new(a=ds([1, 2]))
    x_object = x.embed_schema()
    testing.assert_equal(
        x_object.get_schema(), schema_constants.OBJECT.with_db(db)
    )
    testing.assert_equal(x_object.a, x.a)
    schema_attr = x_object.get_attr('__schema__')
    testing.assert_equal(
        schema_attr == x.get_schema(), ds([arolla.present(), arolla.present()])
    )

  def test_embed_schema_primitive(self):
    testing.assert_equal(
        ds([1, 2, 3]).embed_schema(), ds([1, 2, 3], schema_constants.OBJECT)
    )

  def test_follow(self):
    x = bag().new()
    testing.assert_equal(kde.nofollow._eval(x).follow(), x)
    with self.assertRaisesRegex(ValueError, 'a nofollow schema is required'):
      ds([1, 2, 3]).follow()


class DataSliceMergingTest(parameterized.TestCase):

  def test_set_get_attr(self):
    db = bag()
    x = db.new(abc=ds([42]))
    db2 = bag()
    x2 = db2.new(qwe=ds([57]))

    x.get_schema().xyz = x2.get_schema()
    x.xyz = x2
    testing.assert_equal(x.abc, ds([42]).with_db(db))
    testing.assert_equal(x.abc.get_schema(), schema_constants.INT32.with_db(db))
    testing.assert_equal(x.xyz.qwe, ds([57]).with_db(db))
    testing.assert_equal(
        x.xyz.qwe.get_schema(), schema_constants.INT32.with_db(db)
    )

  def test_set_get_dict_single(self):
    db = bag()
    dct = db.dict()
    dct['a'] = 7
    db2 = bag()
    dct2 = db2.dict()
    dct2['b'] = 5
    dct['obj'] = dct2

    testing.assert_equal(dct['a'], ds(7, schema_constants.OBJECT).with_db(db))
    testing.assert_equal(
        dct['obj']['b'], ds(5, schema_constants.OBJECT).with_db(db)
    )

    ds([dct.as_any(), dct['obj'].as_any()])['c'] = ds(
        [db2.obj(a=1), db2.obj(a=2)]
    )
    testing.assert_equal(dct['c'].a, ds(1).with_db(db))
    testing.assert_equal(dct['obj']['c'].a, ds(2).with_db(db))

  def test_dict_keys_bag_merging(self):
    obj1 = bag().obj(a=7)
    obj2 = bag().obj(a=3)
    dct = bag().dict()
    dct[obj1] = 4
    dct[obj2] = 5
    testing.assert_dicts_keys_equal(
        dct, ds([obj1, obj2], schema_constants.OBJECT)
    )

  def test_set_get_dict_slice(self):
    db = bag()
    keys_ds = db.dict_shaped(jagged_shape.create_shape([2]))
    keys_ds['abc_key'] = 1
    values_ds = db.new(abc_value=ds(['v', 'w']))

    with self.assertRaisesRegex(
        ValueError, 'only supported for Koda List DataItem'
    ):
      keys_ds[ds(['a', 'b'])] = [4, 5]

    with self.assertRaisesRegex(
        ValueError, 'only supported for Koda Dict DataItem'
    ):
      keys_ds[ds(['a', 'b'])] = {'a': 4}

    with self.subTest('only keys have db'):
      db2 = bag()
      dct = db2.dict()
      dct[keys_ds] = 1
      testing.assert_equal(
          dct[keys_ds], ds([1, 1], schema_constants.OBJECT).with_db(db2)
      )
      testing.assert_equal(
          keys_ds.with_db(db2)['abc_key'],
          ds([1, 1], schema_constants.OBJECT).with_db(db2),
      )
      del db2

    with self.subTest('only values have db'):
      db2 = bag()
      dct = db2.dict()
      keys = ds(['a', 'b'])
      dct[keys] = values_ds
      testing.assert_equal(dct[keys].abc_value, ds(['v', 'w']).with_db(db2))
      del db2

    with self.subTest('keys and values have the same db'):
      db2 = bag()
      dct = db2.dict()
      dct[keys_ds] = values_ds
      testing.assert_equal(
          keys_ds.with_db(db2)['abc_key'],
          ds([1, 1], schema_constants.OBJECT).with_db(db2),
      )
      testing.assert_equal(dct[keys_ds].abc_value, ds(['v', 'w']).with_db(db2))
      del db2

    with self.subTest('keys and values have different db'):
      db3 = bag()
      values = db3.new(abc_value=ds(['Y', 'Z']))
      db2 = bag()
      dct = db2.dict()
      dct[keys_ds] = values
      testing.assert_equal(
          keys_ds.with_db(db2)['abc_key'],
          ds([1, 1], schema_constants.OBJECT).with_db(db2),
      )
      testing.assert_equal(dct[keys_ds].abc_value, ds(['Y', 'Z']).with_db(db2))
      del db2

  def test_set_get_list_single(self):
    db = bag()
    lst = db.list()
    lst.append(7)
    db2 = bag()
    lst2 = db2.list()
    lst2.append(5)
    lst[0] = lst2

    testing.assert_equal(lst[0][0], ds(5, schema_constants.OBJECT).with_db(db))

  def test_append_list_single(self):
    db = bag()
    lst = db.list(
        # TODO: Assigning schema to OBJECT breaks the [:]
        # operator below.
        item_schema=schema_constants.ANY
    )
    lst2 = bag().list([5])
    lst3 = bag().list([6])
    lst.append(lst2)
    lst.append(ds([lst3]))

    testing.assert_equal(lst[0][0], ds(5).as_any().with_db(db))
    testing.assert_equal(lst[1][0], ds(6).as_any().with_db(db))

  def test_replace_in_list_single(self):
    db = bag()
    lst = db.list()
    lst.append(7)
    db2 = bag()
    lst2 = db2.list([5])
    lst2_ext = db2.list([lst2])
    lst[:] = lst2_ext[:]
    lst[1:] = ds([
        bag().obj(a=1),
        bag().obj(a=2),
    ])
    testing.assert_equal(lst[0][0], ds(5, schema_constants.INT32).with_db(db))
    testing.assert_equal(
        lst[1:].a, ds([1, 2], schema_constants.INT32).with_db(db)
    )

  def test_get_set_schema(self):
    db = bag()
    obj = db.new(a=1)
    db2 = bag()
    obj2 = obj.with_db(db2)
    obj2.get_schema().x = obj.get_schema()

    testing.assert_equal(
        obj2.get_schema().x.a, schema_constants.INT32.with_db(obj2.db)
    )


class DataSliceFallbackTest(parameterized.TestCase):

  @parameterized.parameters(
      (None,), ([1, 2, 3],), (bag().new(x=1),)
  )
  def test_errors(self, db):
    with self.assertRaisesRegex(TypeError, 'expecting db to be a DataBag'):
      ds([1, 2, 3]).with_fallback(db)

  def test_immutable(self):
    db = bag()
    x = db.new(q=1)
    x.get_schema().w = schema_constants.INT32
    x_fb = x.with_fallback(db)
    with self.assertRaisesRegex(ValueError, 'immutable'):
      x_fb.get_schema().w = schema_constants.INT32
      x_fb.w = x_fb.q + 1

  def test_immutable_with_merging(self):
    db = bag()
    x = db.new(q=1)
    x.get_schema().w = schema_constants.INT32
    x_fb = x.with_fallback(db)
    db2 = bag()
    x2 = db2.new(q=1)
    with self.assertRaisesRegex(ValueError, 'immutable'):
      x_fb.get_schema().w = schema_constants.INT32
      x_fb.w = x2.q + 1

  def test_get_attr_no_original_db(self):
    db = bag()
    x = db.new(abc=ds([3.14, None]))
    x = x.with_db(None)
    x = x.with_fallback(db)
    testing.assert_allclose(x.abc, ds([3.14, None]).with_db(x.db))

  def test_get_attr(self):
    db = bag()
    x = db.new(abc=ds([3.14, None]))
    x.get_schema().xyz = schema_constants.FLOAT64
    x.xyz = ds([2.71, None], schema_constants.FLOAT64)

    fb_db = bag()
    fb_x = x.with_db(fb_db)
    fb_x.get_schema().abc = schema_constants.FLOAT32
    fb_x.abc = ds([None, 2.71])

    merged_x = x.with_fallback(fb_db)

    testing.assert_allclose(merged_x.abc, ds([3.14, 2.71]).with_db(merged_x.db))
    testing.assert_allclose(
        merged_x.xyz,
        ds([2.71, None], schema_constants.FLOAT64).with_db(merged_x.db),
    )

    # update new DataBag
    new_db = bag()
    new_x = x.with_db(new_db).as_any()
    new_x.xyz = ds([None, 3.14], schema_constants.FLOAT64)
    merged_x = new_x.with_fallback(db).with_fallback(fb_db)
    testing.assert_allclose(
        merged_x.xyz,
        ds([2.71, 3.14], schema_constants.FLOAT64)
        .as_any()
        .with_db(merged_x.db),
    )
    testing.assert_allclose(
        x.xyz, ds([2.71, None], schema_constants.FLOAT64).with_db(x.db)
    )

    # update original DataBag
    x.xyz = ds([1.61, None], schema_constants.FLOAT64)
    testing.assert_allclose(
        merged_x.xyz,
        ds([1.61, 3.14], schema_constants.FLOAT64)
        .as_any()
        .with_db(merged_x.db),
    )

  def test_get_attr_mixed_type(self):
    db = bag()
    x = db.new(abc=ds([314, None])).as_any()
    fb_db = bag()
    fb_x = x.with_db(fb_db)
    fb_x.abc = ds([None, '2.71'])
    merged_x = x.with_fallback(fb_db)
    testing.assert_equal(
        merged_x.abc, ds([314, '2.71']).as_any().with_db(merged_x.db),
    )

  def test_dict(self):
    db = bag()
    x = db.dict_shaped(jagged_shape.create_shape([2]))
    x['abc'] = ds([3.14, None])
    x['xyz'] = ds([2.71, None])

    fb_db = bag()
    fb_x = x.with_db(fb_db)
    x.get_schema().with_db(fb_db).set_attr(
        '__keys__', x.get_schema().get_attr('__keys__')
    )
    fb_x['abc'] = ds([None, 2.71])
    fb_x['qwe'] = ds([None, 'pi'])
    fb_x['asd'] = ds(['e', None])

    merged_x = x.with_fallback(fb_db)

    testing.assert_dicts_keys_equal(
        merged_x,
        ds([['abc', 'xyz', 'asd'], ['abc', 'qwe']], schema_constants.OBJECT),
    )
    testing.assert_allclose(
        merged_x['abc'],
        ds([3.14, 2.71], schema_constants.OBJECT).with_db(merged_x.db),
    )
    testing.assert_allclose(
        merged_x['xyz'],
        ds([2.71, None], schema_constants.OBJECT).with_db(merged_x.db),
    )

    new_db = bag()
    merged_x = merged_x.with_db(new_db)
    merged_x.get_schema().with_db(new_db).set_attr(
        '__keys__', x.get_schema().get_attr('__keys__')
    )
    merged_x['xyz'] = ds([None, 3.14])
    merged_x = merged_x.with_fallback(db).with_fallback(fb_db)
    testing.assert_allclose(
        merged_x['xyz'],
        ds([2.71, 3.14], schema_constants.OBJECT).with_db(merged_x.db),
    )

  def test_deep_fallbacks(self):
    cnt = 100
    dbs = [bag() for _ in range(cnt)]
    dct = dbs[0].dict()
    dct_schema = dct.get_schema()
    obj = dbs[0].new(q=1)
    merged_db = bag()
    for i, db in enumerate(dbs):
      dct_schema.with_db(db).set_attr(
          '__keys__', dct_schema.get_attr('__keys__')
      )
      dct.with_db(db)[f'd{i}'] = i
      setattr(obj.get_schema().with_db(db), f'a{i}', schema_constants.INT32)
      setattr(obj.with_db(db), f'a{i}', -i)
      merged_db = dct.with_db(merged_db).with_fallback(db).db

    dct = dct.with_db(merged_db)
    testing.assert_dicts_keys_equal(
        dct, ds([f'd{i}' for i in range(cnt)], schema_constants.OBJECT)
    )
    obj = obj.with_db(merged_db)
    for i in range(cnt):
      testing.assert_equal(
          dct[f'd{i}'], ds(i, schema_constants.OBJECT).with_db(dct.db)
      )
      testing.assert_equal(getattr(obj, f'a{i}'), ds(-i).with_db(obj.db))

  def test_disabled_data_item_magic_methods(self):
    with self.assertRaisesRegex(
        TypeError, '__bool__ disabled for data_slice.DataSlice'
    ):
      bool(ds([arolla.unit()]))
    with self.assertRaisesRegex(
        TypeError,
        'slice indices must be integers or None or have an __index__ method',
    ):
      _ = [4, 5, 6][ds([4]) : 7]
    with self.assertRaisesRegex(
        TypeError, 'argument must be a string or a real number'
    ):
      float(ds([3.14]))

  def test_get_present_count(self):
    self.assertEqual(ds(57).get_present_count(), 1)
    self.assertEqual(ds(None).get_present_count(), 0)
    self.assertEqual(ds([3.14, None, 57.0]).get_present_count(), 2)

  def test_get_size(self):
    self.assertEqual(ds(57).get_size(), 1)
    self.assertEqual(ds(None).get_size(), 1)
    self.assertEqual(ds([3.14, None, 57.0]).get_size(), 3)
    self.assertEqual(ds([[1, 2], [3, None], [None]]).get_size(), 5)

  def test_get_ndim(self):
    self.assertEqual(ds(57).get_ndim(), 0)
    self.assertEqual(ds([1, 2, 3]).get_ndim(), 1)
    self.assertEqual(ds([[1, 2], [3, 4, 5]]).get_ndim(), 2)
    self.assertEqual(ds([[[[[]]]]]).get_ndim(), 5)

  # More comprehensive tests are in the test_core_subslice.py.
  @parameterized.parameters(
      # x.ndim=1
      (ds([1, 2, 3]), [ds(1)], ds(2)),
      (ds([1, 2, 3]), [ds([1, 0])], ds([2, 1])),
      (ds([1, 2, 3]), [slice(0, 2)], ds([1, 2])),
      (ds([1, 2, 3]), [slice(None, 2)], ds([1, 2])),
      (ds([1, 2, 3]), [...], ds([1, 2, 3])),
      # x.ndim=2
      (ds([[1, 2], [3], [4, 5, 6]]), [ds(0), ds(-1)], ds(2)),
      (
          ds([[1, 2], [3], [4, 5, 6]]),
          [slice(1, 3), slice(1, -1)],
          ds([[], [5]]),
      ),
      (ds([[1, 2], [3], [4, 5, 6]]), [..., ds(0), ds(-1)], ds(2)),
      (ds([[1, 2], [3], [4, 5, 6]]), [..., slice(1, 3)], ds([[2], [], [5, 6]])),
      # Mixed types
      (
          ds([[1, 'a'], [3], [4, 'b', 6]]),
          [..., ds(1)],
          ds(['a', None, 'b'], schema_constants.OBJECT),
      ),
      # Out-of-bound indices
      (ds([[1, 2], [3], [4, 5, 6]]), [..., ds(2)], ds([None, None, 6])),
  )
  def test_subslice(self, x, slices, expected):
    testing.assert_equal(x.S[*slices], expected)


if __name__ == '__main__':
  absltest.main()
