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

"""Tests for set_attrs."""

import re

from absl.testing import absltest
from koladata.exceptions import exceptions
from koladata.functions import functions as fns
from koladata.testing import testing
from koladata.types import data_slice
from koladata.types import schema_constants

ds = data_slice.DataSlice.from_vals


class SetAttrsTest(absltest.TestCase):

  def test_entity(self):
    x = fns.new(a=ds(1, schema_constants.INT64), b='a')
    fns.set_attrs(x, a=2, b='abc')
    testing.assert_equal(
        x.a, ds(2, schema_constants.INT64).with_bag(x.get_bag())
    )
    testing.assert_equal(x.b, ds('abc').with_bag(x.get_bag()))

  def test_incomaptible_schema_entity(self):
    x = fns.new(a=1, b='a')
    with self.assertRaisesRegex(
        exceptions.KodaError,
        re.escape(r"""the schema for attribute 'b' is incompatible.

Expected schema for 'b': STRING
Assigned schema for 'b': BYTES

To fix this, explicitly override schema of 'b' in the original schema. For example,
schema.b = <desired_schema>"""),
    ):
      fns.set_attrs(x, a=2, b=b'abc')

  def test_update_schema_entity(self):
    x = fns.new(a=1, b='a')
    fns.set_attrs(x, a=2, b=b'abc', update_schema=True)
    testing.assert_equal(x.a, ds(2).with_bag(x.get_bag()))
    testing.assert_equal(x.b, ds(b'abc').with_bag(x.get_bag()))

  def test_object(self):
    x = fns.obj()
    fns.set_attrs(x, a=2, b='abc')
    testing.assert_equal(x.a, ds(2).with_bag(x.get_bag()))
    testing.assert_equal(x.b, ds('abc').with_bag(x.get_bag()))

  def test_incomaptible_schema_object(self):
    x_schema = fns.new(a=1, b='a').get_schema()
    x = fns.obj()
    x.set_attr('__schema__', x_schema)
    with self.assertRaisesRegex(
        exceptions.KodaError,
        re.escape(r"""the schema for attribute 'b' is incompatible.

Expected schema for 'b': STRING
Assigned schema for 'b': BYTES

To fix this, explicitly override schema of 'b' in the Object schema. For example,
foo.get_obj_schema().b = <desired_schema>"""),
    ):
      fns.set_attrs(x, a=2, b=b'abc')

  def test_update_schema_object(self):
    x_schema = fns.new(a=1, b='a').get_schema()
    x = fns.obj()
    x.set_attr('__schema__', x_schema)
    fns.set_attrs(x, a=2, b=b'abc', update_schema=True)
    testing.assert_equal(x.a, ds(2).with_bag(x.get_bag()))
    testing.assert_equal(x.b, ds(b'abc').with_bag(x.get_bag()))


if __name__ == '__main__':
  absltest.main()
