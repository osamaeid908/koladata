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
from koladata.functions import functions as fns
from koladata.types import data_slice

ds = data_slice.DataSlice.from_vals


class SetAttrTest(absltest.TestCase):

  def test_entity(self):
    db = fns.bag()
    x = db.new(xyz=3.14)
    self.assertTrue(x.has_attr('xyz'))
    fns.del_attr(x, 'xyz')
    self.assertFalse(x.has_attr('xyz'))

  def test_object(self):
    db = fns.bag()
    x = db.obj(xyz=3.14)
    self.assertTrue(x.has_attr('xyz'))
    fns.del_attr(x, 'xyz')
    self.assertFalse(x.has_attr('xyz'))

  def test_fails_on_non_existing_attr(self):
    db = fns.bag()
    x = db.obj(xyz=3.14)
    with self.assertRaisesRegex(
        ValueError,
        "the attribute 'foo' is missing",
    ):
      fns.del_attr(x, 'foo')


if __name__ == '__main__':
  absltest.main()
