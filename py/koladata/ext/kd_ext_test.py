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
from koladata import kd
from koladata.ext import kd_ext
from koladata.testing import testing


class KdExtTest(absltest.TestCase):

  def test_contains_modules(self):
    modules = dir(kd_ext)
    self.assertIn('npkd', modules)
    self.assertIn('pdkd', modules)
    self.assertIn('nested_data', modules)

  def test_functor_factories(self):
    testing.assert_equal(kd_ext.Fn(lambda: 5)(), kd.item(5))
    testing.assert_equal(
        kd_ext.PyFn(lambda x: 5 if x == 2 else 10)(2), kd.item(5)
    )


if __name__ == '__main__':
  absltest.main()
