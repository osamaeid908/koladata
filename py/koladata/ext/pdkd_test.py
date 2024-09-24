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
from koladata import kd
from koladata.ext import pdkd
from koladata.testing import testing
from koladata.types import schema_constants
import numpy as np
import pandas as pd

S = kd.S


def to_itemid_str(x):
  return str(x.as_itemid())


def map_to_itemid_str(items):
  return [str(x.as_itemid()) for x in items]


class NpkdTest(parameterized.TestCase):

  def test_from_dataframe(self):

    with self.subTest('primitive df'):
      df = pd.DataFrame({'x': [1, 2, 3]})
      ds = pdkd.from_dataframe(df)
      testing.assert_equal(
          ds.x, kd.slice([1, 2, 3], dtype=schema_constants.INT64).with_db(ds.db)
      )

    with self.subTest('multi-dimensional int df'):
      index = pd.MultiIndex.from_arrays([[0, 0, 1, 3, 3], [0, 1, 0, 0, 1]])
      df = pd.DataFrame({'x': [1, 2, 3, 4, 5]}, index=index)
      ds = pdkd.from_dataframe(df)
      testing.assert_equal(
          ds.x,
          kd.slice(
              [[1, 2], [3], [], [4, 5]], dtype=schema_constants.INT64
          ).with_db(ds.db),
      )

    with self.subTest('non-primitive df'):
      df = pd.DataFrame({'self_': ['$1', '$2', '$3'], 'x': [1, 2, 3]})
      ds = pdkd.from_dataframe(df)
      self.assertCountEqual(dir(ds), ['self_', 'x'])
      self.assertNotEqual(ds.get_schema(), schema_constants.OBJECT)
      testing.assert_equal(
          ds.get_attr('self_'), kd.slice(['$1', '$2', '$3']).with_db(ds.db)
      )
      testing.assert_equal(
          ds.x, kd.slice([1, 2, 3], dtype=schema_constants.INT64).with_db(ds.db)
      )

    with self.subTest('non-primitive df with as_obj set to True'):
      df = pd.DataFrame({'self_': ['$1', '$2', '$3'], 'x': [1, 2, 3]})
      ds = pdkd.from_dataframe(df, as_obj=True)
      self.assertCountEqual(dir(ds), ['self_', 'x'])
      self.assertEqual(ds.get_schema(), schema_constants.OBJECT)
      testing.assert_equal(
          ds.get_attr('self_'), kd.slice(['$1', '$2', '$3']).with_db(ds.db)
      )
      testing.assert_equal(
          ds.x, kd.slice([1, 2, 3], dtype=schema_constants.INT64).with_db(ds.db)
      )

    with self.subTest('empty df'):
      with self.assertRaisesRegex(ValueError, 'DataFrame has no columns'):
        _ = pdkd.from_dataframe(pd.DataFrame())

  def test_to_dataframe(self):

    with self.subTest('primitive ds'):
      ds = kd.slice([1, 2, 3])
      df = pdkd.to_dataframe(ds)
      self.assertNotIsInstance(df.index, pd.DataFrame)
      self.assertCountEqual(df.columns, ['self_'])
      self.assertCountEqual(df['self_'], [1, 2, 3])

    with self.subTest('mixed ds with primitive and object'):
      ds = kd.slice([kd.obj(x=1), 'abc'])
      with self.assertRaisesRegex(
          ValueError, 'getting attributes of primitives is not allowed'
      ):
        _ = pdkd.to_dataframe(ds)

    with self.subTest('primitive ds with databag'):
      ds = kd.slice([1, 2, 3]).with_db(kd.bag())
      df = pdkd.to_dataframe(ds)
      self.assertNotIsInstance(df.index, pd.DataFrame)
      self.assertCountEqual(df.columns, ['self_'])
      self.assertCountEqual(df['self_'], [1, 2, 3])

    with self.subTest('int ds as OBJECT schema'):
      ds = kd.slice([1, 2, 3]).with_schema(kd.OBJECT)
      df = pdkd.to_dataframe(ds)
      self.assertCountEqual(df.columns, ['self_'])
      self.assertCountEqual(df['self_'], [1, 2, 3])

    with self.subTest('int ds as ANY schema'):
      ds = kd.slice([1, 2, 3]).as_any()
      df = pdkd.to_dataframe(ds)
      self.assertCountEqual(df.columns, ['self_'])
      self.assertCountEqual(df['self_'], [1, 2, 3])

    with self.subTest('int DataItem'):
      ds = kd.item(1)
      df = pdkd.to_dataframe(ds)
      self.assertNotIsInstance(df.index, pd.DataFrame)
      self.assertCountEqual(df.columns, ['self_'])
      self.assertCountEqual(df['self_'], [1])

    with self.subTest('multi-dimensional int ds'):
      ds = kd.slice([[1, 2], [3], [], [4, 5]])
      df = pdkd.to_dataframe(ds)
      self.assertCountEqual(df.columns, ['self_'])
      self.assertCountEqual(df['self_'], [1, 2, 3, 4, 5])
      self.assertIsInstance(df.index, pd.MultiIndex)
      self.assertCountEqual(df.index.get_level_values(0), [0, 0, 1, 3, 3])
      self.assertCountEqual(df.index.get_level_values(1), [0, 1, 0, 0, 1])

    with self.subTest('list ds'):
      l1 = kd.list()
      l2 = kd.list()
      l3 = kd.list()
      ds = kd.slice([l1, l2, l3])
      df = pdkd.to_dataframe(ds)
      self.assertCountEqual(df.columns, ['self_'])
      self.assertEqual(
          map_to_itemid_str(df['self_']), map_to_itemid_str([l1, l2, l3])
      )

    with self.subTest('dict ds'):
      d1 = kd.dict()
      d2 = kd.dict()
      d3 = kd.dict()
      ds = kd.slice([d1, d2, d3])
      df = pdkd.to_dataframe(ds)
      self.assertCountEqual(df.columns, ['self_'])
      self.assertEqual(
          map_to_itemid_str(df['self_']), map_to_itemid_str([d1, d2, d3])
      )

    with self.subTest('entity ds'):
      ds = kd.new(x=kd.slice([1, 2, 3]), y=kd.slice(['a', 'b', 'c']))
      df = pdkd.to_dataframe(ds)
      self.assertCountEqual(df.columns, ['self_', 'x', 'y'])
      self.assertCountEqual(df['x'], [1, 2, 3])
      self.assertCountEqual(df['y'], ['a', 'b', 'c'])

    with self.subTest('entity ds with attrs'):
      ds = kd.new(x=kd.slice([1, 2, 3]), y=kd.slice(['a', 'b', 'c']))
      df = pdkd.to_dataframe(ds, cols=['x'])
      self.assertCountEqual(df.columns, ['x'])
      self.assertCountEqual(df['x'], [1, 2, 3])

    with self.subTest('obj ds'):
      ds = kd.obj(x=kd.slice([1, 2, 3]), y=kd.slice(['a', 'b', 'c']))
      df = pdkd.to_dataframe(ds)
      self.assertCountEqual(df.columns, ['self_', 'x', 'y'])
      self.assertCountEqual(df['x'], [1, 2, 3])
      self.assertCountEqual(df['y'], ['a', 'b', 'c'])

    # missing int values are replaced with 0
    with self.subTest('obj ds with different attrs int'):
      ds = kd.slice([kd.obj(x=1, y='a'), kd.obj(x=2), kd.obj(y='c')])
      df = pdkd.to_dataframe(ds)
      self.assertCountEqual(df.columns, ['self_', 'x', 'y'])
      self.assertSameElements(df['x'], [1, 2, 0])
      self.assertCountEqual(df['y'], ['a', None, 'c'])

    # missing float values are replaced with nan
    with self.subTest('obj ds with different attrs float'):
      ds = kd.slice([kd.obj(x=1.0, y='a'), kd.obj(x=2.0), kd.obj(y='c')])
      df = pdkd.to_dataframe(ds)
      self.assertCountEqual(df.columns, ['self_', 'x', 'y'])
      self.assertCountEqual(np.isnan(df['x']), [False, False, True])
      self.assertEqual(df['x'][0], 1)
      self.assertEqual(df['x'][1], 2)
      self.assertCountEqual(df['y'], ['a', None, 'c'])

    with self.subTest('obj ds with different attrs and cols'):
      ds = kd.slice([kd.obj(x=1, y='a'), kd.obj(x=2), kd.obj(x=3, y='c')])
      df = pdkd.to_dataframe(ds, cols=['x', S.get_attr('y', default=None)])
      expected_optional_column = (
          "kde.get_attr(S, DataItem('y', schema: TEXT), DataItem(None, schema:"
          ' NONE))'
      )
      self.assertCountEqual(df.columns, ['x', expected_optional_column])
      self.assertCountEqual(df['x'], [1, 2, 3])
      self.assertCountEqual(df[expected_optional_column], ['a', None, 'c'])

      ds1 = ds.fork_db()
      ds1.y = ds1.get_attr('y', default=None)
      df = pdkd.to_dataframe(ds1, cols=['x', 'y'])
      self.assertCountEqual(df.columns, ['x', 'y'])
      self.assertCountEqual(df['x'], [1, 2, 3])
      self.assertCountEqual(df['y'], ['a', None, 'c'])

      with self.assertRaisesRegex(ValueError, "the attribute 'y' is missing"):
        _ = pdkd.to_dataframe(ds, cols=['y'])

      with self.assertRaisesRegex(ValueError, "the attribute 'z' is missing"):
        _ = pdkd.to_dataframe(ds, cols=['z'])

    with self.subTest('entity ds without db'):
      ds = kd.new(x=kd.slice([1, 2, 3]), y=kd.slice(['a', 'b', 'c'])).no_db()
      df = pdkd.to_dataframe(ds)
      self.assertCountEqual(df.columns, ['self_'])
      self.assertCountEqual(df['self_'], ds.internal_as_py())

    with self.subTest('use Expr as columns'):
      ds = kd.new(x=kd.slice([1, 2, 3]), y=kd.slice([4, 5, 6]))
      df = pdkd.to_dataframe(ds, cols=[S.x, 'y', S.x + S.y])
      self.assertCountEqual(df.columns, ['S.x', 'y', 'S.x + S.y'])
      self.assertCountEqual(df['S.x'], [1, 2, 3])
      self.assertCountEqual(df['y'], [4, 5, 6])
      self.assertCountEqual(df['S.x + S.y'], [5, 7, 9])

    with self.subTest('use named Expr as columns'):
      ds = kd.new(x=kd.slice([1, 2, 3]), y=kd.slice([4, 5, 6]))
      df = pdkd.to_dataframe(
          ds, cols=[S.x.with_name('my_x'), (S.x + S.y).with_name('my_sum')]
      )
      self.assertCountEqual(df.columns, ['my_x', 'my_sum'])
      self.assertCountEqual(df['my_x'], [1, 2, 3])
      self.assertCountEqual(df['my_sum'], [5, 7, 9])

    with self.subTest('broadcast to common shape'):
      ds = kd.new(
          x=kd.slice([1, 2, 3]),
          y=kd.list(kd.new(z=kd.slice([[4, 5], [], [6]]))),
      )
      df = pdkd.to_dataframe(ds, cols=[S.x, S.y[:].z])
      self.assertCountEqual(df.columns, ['S.x', 'S.y[:].z'])
      self.assertCountEqual(df['S.x'], [1, 1, 3])
      self.assertCountEqual(df['S.y[:].z'], [4, 5, 6])

      self.assertIsInstance(df.index, pd.MultiIndex)
      self.assertCountEqual(df.index.get_level_values(0), [0, 0, 2])
      self.assertCountEqual(df.index.get_level_values(1), [0, 1, 0])

    with self.subTest('invalid attr'):
      ds = kd.new(x=kd.slice([1, 2, 3]), y=kd.slice(['a', 'b', 'c']))
      with self.assertRaisesRegex(ValueError, "the attribute 'z' is missing"):
        _ = pdkd.to_dataframe(ds, cols=['z'])

    with self.subTest('invalid column'):
      ds = kd.new(x=kd.slice([1, 2, 3]), y=kd.slice(['a', 'b', 'c']))
      with self.assertRaisesRegex(ValueError, 'Unsupported attr type'):
        _ = pdkd.to_dataframe(ds, cols=[1])

    with self.subTest('invalid Expr'):
      ds = kd.new(x=kd.slice([1, 2, 3]), y=kd.slice(['a', 'b', 'c']))
      with self.assertRaisesRegex(
          ValueError, 'Cannot evaluate S.z on DataSlice'
      ):
        _ = pdkd.to_dataframe(ds, cols=[S.z])

    with self.subTest('broadcast to common shape'):
      ds = kd.new(
          x=kd.list(kd.new(z=kd.slice([[4], [5], [6]]))),
          y=kd.list(kd.new(z=kd.slice([[4, 5], [], [6]]))),
      )
      with self.assertRaisesRegex(
          ValueError, 'All columns must have compatible shapes'
      ):
        _ = pdkd.to_dataframe(ds, cols=[S.x[:].z, S.y[:].z])


if __name__ == '__main__':
  absltest.main()