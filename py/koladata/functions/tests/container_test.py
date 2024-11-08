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

"""Tests for container.

Currently container operation is just an alias for obj, so this test contains
only one basic check to avoid duplicating all the tests for obj.
"""

from absl.testing import absltest
from koladata.functions import functions as fns
from koladata.testing import testing
from koladata.types import data_slice


ds = data_slice.DataSlice.from_vals


class ContainerTest(absltest.TestCase):

  def test_set_get_attr(self):
    c = fns.container()
    c.a = 42
    testing.assert_equal(c.a.no_bag(), ds(42))
    c.b = ds(42)
    testing.assert_equal(c.b.no_bag(), ds(42))

  def test_autobox_python_attrs(self):
    c = fns.container()
    c.lst = [1, 2]
    testing.assert_equal(c.lst[:].no_bag(), ds([1, 2]))
    c.dct = {'a': 42, 'b': 12}
    testing.assert_dicts_keys_equal(c.dct, ds(['a', 'b']))

  def test_mutable(self):
    self.assertTrue(fns.container().is_mutable())
    self.assertTrue(fns.container(db=fns.bag()).is_mutable())
    self.assertTrue(fns.container(db=fns.bag(), a=42).is_mutable())

  def test_db_arg(self):
    db = fns.bag()
    c = fns.container(db=db)
    testing.assert_equal(c.db, db)

    c = fns.container(db=db, a=42)
    testing.assert_equal(c.db, db)
    testing.assert_equal(c.a.no_bag(), ds(42))

  def test_non_data_item(self):
    with self.assertRaisesRegex(
        ValueError,
        'trying to assign a slice with 1 dimensions to a slice with only 0 '
        'dimensions'
    ):
      fns.container(a=ds([1, 2]))


if __name__ == '__main__':
  absltest.main()