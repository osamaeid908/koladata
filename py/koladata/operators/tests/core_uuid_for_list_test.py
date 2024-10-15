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
from koladata.testing import testing
from koladata.types import data_slice
from koladata.types import qtypes

I = input_container.InputContainer('I')
M = arolla.M
ds = data_slice.DataSlice.from_vals
DATA_SLICE = qtypes.DATA_SLICE
kde = kde_operators.kde


class KodaUuidForListTest(parameterized.TestCase):

  @parameterized.parameters(
      (
          '',
          dict(a=ds(1), b=ds(2)),
          '',
          dict(b=ds(2), a=ds(1)),
      ),
      (
          'specified_seed',
          dict(a=ds(1), b=ds(2)),
          'specified_seed',
          dict(b=ds(2), a=ds(1)),
      ),
      (
          ds('specified_seed'),
          dict(a=ds(1), b=ds(2)),
          ds('specified_seed'),
          dict(b=ds(2), a=ds(1)),
      ),
      (
          '',
          dict(a=ds([1, 2, 3]), b=ds(2)),
          '',
          dict(b=ds(2), a=ds([1, 2, 3])),
      ),
  )
  def test_equal(self, lhs_seed, lhs_kwargs, rhs_seed, rhs_kwargs):
    lhs = expr_eval.eval(kde.core.uuid_for_list(seed=lhs_seed, **lhs_kwargs))
    rhs = expr_eval.eval(kde.core.uuid_for_list(seed=rhs_seed, **rhs_kwargs))
    testing.assert_equal(lhs, rhs)

  @parameterized.parameters(
      (
          '',
          dict(a=ds(1), b=ds(2)),
          '',
          dict(a=ds(2), b=ds(1)),
      ),
      (
          'seed1',
          dict(a=ds(1), b=ds(2)),
          'seed2',
          dict(a=ds(1), b=ds(2)),
      ),
  )
  def test_not_equal(self, lhs_seed, lhs_kwargs, rhs_seed, rhs_kwargs):
    lhs = expr_eval.eval(kde.core.uuid_for_list(seed=lhs_seed, **lhs_kwargs))
    rhs = expr_eval.eval(kde.core.uuid_for_list(seed=rhs_seed, **rhs_kwargs))
    self.assertNotEqual(lhs.fingerprint, rhs.fingerprint)

  def test_default_seed(self):
    lhs = expr_eval.eval(kde.core.uuid_for_list(a=ds(1), b=ds(2)))
    rhs = expr_eval.eval(kde.core.uuid_for_list(seed='', a=ds(1), b=ds(2)))
    self.assertEqual(lhs.fingerprint, rhs.fingerprint)

  def test_no_args(self):
    lhs = expr_eval.eval(kde.core.uuid_for_list())
    rhs = expr_eval.eval(kde.core.uuid_for_list(seed=''))
    self.assertEqual(lhs.fingerprint, rhs.fingerprint)

  def test_seed_keywod_only_args(self):
    with self.assertRaisesWithLiteralMatch(
        TypeError, 'expected 0 positional arguments but 1 were given'
    ):
      _ = expr_eval.eval(kde.core.uuid_for_list(ds('a')))

  @parameterized.parameters(
      (
          '',
          dict(a=ds([1, 2, 3]), b=ds([1, 2])),
          'shapes are not compatible',
      ),
      (
          ds(['seed1', 'seed2']),
          dict(a=ds([1, 2, 3]), b=ds([1, 2, 3])),
          'requires seed to be DataItem holding Text, got DataSlice',
      ),
      (
          0,
          dict(a=ds([1, 2, 3]), b=ds([1, 2, 3])),
          (
              r'requires seed to be DataItem holding Text, got DataItem\(0'
              r', schema: INT32\)'
          ),
      ),
      (
          '',
          dict(a=ds([1, 2, 3]), b=arolla.text('')),
          (
              'expected all arguments to be DATA_SLICE, got kwargs:'
              ' namedtuple<a=DATA_SLICE,b=TEXT>'
          ),
      ),
  )
  def test_error(self, seed, kwargs, err_regex):
    with self.assertRaisesRegex(
        ValueError,
        err_regex,
    ):
      _ = expr_eval.eval(kde.core.uuid_for_list(seed=seed, **kwargs))

  def test_non_data_slice_binding(self):
    with self.assertRaisesRegex(
        ValueError,
        'expected all arguments to be DATA_SLICE, got kwargs:'
        ' namedtuple<a=DATA_SLICE,b=UNSPECIFIED>',
    ):
      _ = kde.core.uuid_for_list(
          a=ds(1),
          b=arolla.unspecified(),
      )

  def test_view(self):
    self.assertTrue(
        view.has_data_slice_view(kde.core.uuid_for_list(seed=I.seed))
    )

  def test_alias(self):
    self.assertTrue(
        optools.equiv_to_op(kde.core.uuid_for_list, kde.uuid_for_list)
    )

  def test_repr(self):
    self.assertEqual(
        repr(kde.core.uuid_for_list(a=I.a)),
        "kde.core.uuid_for_list(seed=DataItem('', schema: TEXT), a=I.a)",
    )


if __name__ == '__main__':
  absltest.main()
