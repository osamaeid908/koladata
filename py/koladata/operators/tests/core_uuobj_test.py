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

"""Tests for koda.core.uuobj."""

from absl.testing import absltest
from absl.testing import parameterized
from arolla import arolla
from koladata.expr import expr_eval
from koladata.expr import input_container
from koladata.expr import view
from koladata.operators import kde_operators
from koladata.testing import testing
from koladata.types import data_slice
from koladata.types import qtypes

I = input_container.InputContainer('I')
M = arolla.M
ds = data_slice.DataSlice.from_vals
DATA_SLICE = qtypes.DATA_SLICE
kde = kde_operators.kde


class KodaUuObjTest(parameterized.TestCase):

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
      (
          '',
          dict(a=ds([1, None, 3]), b=ds(2)),
          '',
          dict(b=ds(2), a=ds([1, None, 3])),
      ),
      (
          '',
          dict(a=ds([1, 2, 3]), b=2),
          '',
          dict(b=2, a=ds([1, 2, 3])),
      ),
  )
  def test_equal(self, lhs_seed, lhs_kwargs, rhs_seed, rhs_kwargs):
    lhs = expr_eval.eval(kde.core._uuobj(lhs_seed, **lhs_kwargs))
    rhs = expr_eval.eval(kde.core._uuobj(rhs_seed, **rhs_kwargs))
    # Check that required attributes are present.
    for attr_name, val in lhs_kwargs.items():
      testing.assert_equal(
          getattr(lhs, attr_name), ds(val).expand_to(lhs).with_db(lhs.db)
      )
    for attr_name, val in rhs_kwargs.items():
      testing.assert_equal(
          getattr(rhs, attr_name), ds(val).expand_to(rhs).with_db(rhs.db)
      )
    testing.assert_equal(lhs, rhs.with_db(lhs.db))

  @parameterized.parameters(
      (
          '',
          dict(a=ds(1), b=ds(2)),
          '',
          dict(a=ds(1), c=ds(2))
      ),
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
    lhs = expr_eval.eval(kde.core._uuobj(lhs_seed, **lhs_kwargs))
    rhs = expr_eval.eval(kde.core._uuobj(rhs_seed, **rhs_kwargs))
    self.assertNotEqual(lhs.fingerprint, rhs.with_db(lhs.db).fingerprint)

  def test_default_seed(self):
    lhs = expr_eval.eval(kde.core._uuobj(a=ds(1), b=ds(2)))
    rhs = expr_eval.eval(kde.core._uuobj('', a=ds(1), b=ds(2)))
    testing.assert_equal(lhs, rhs.with_db(lhs.db))

  def test_no_args(self):
    lhs = expr_eval.eval(kde.core._uuobj())
    rhs = expr_eval.eval(kde.core._uuobj(''))
    testing.assert_equal(lhs, rhs.with_db(lhs.db))

  def test_seed_works_as_kwarg(self):
    lhs = expr_eval.eval(kde.core._uuobj(ds('seed'), a=ds(1), b=ds(2)))
    rhs = expr_eval.eval(kde.core._uuobj(a=ds(1), b=ds(2), seed=ds('seed')))
    testing.assert_equal(lhs, rhs.with_db(lhs.db))

  def test_db_adoption(self):
    a = expr_eval.eval(kde.core._uuobj(a=1))
    b = expr_eval.eval(kde.core._uuobj(a=a))
    testing.assert_equal(b.a.a, ds(1).with_db(b.db))

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
  )
  def test_error(self, seed, kwargs, err_regex):
    with self.assertRaisesRegex(
        ValueError,
        err_regex,
    ):
      _ = expr_eval.eval(kde.core._uuobj(seed, **kwargs))

  def test_non_data_slice_binding(self):
    with self.assertRaisesRegex(
        ValueError,
        'object with unsupported type: "Unspecified',
    ):
      _ = kde.core._uuobj(
          a=ds(1),
          b=arolla.unspecified(),
      )

  def test_view(self):
    self.assertTrue(view.has_data_slice_view(kde.core._uuobj(I.seed)))

  # TODO: Uncomment, when this operator becomes public and we add
  # the alias back.
  # def test_alias(self):
  #   self.assertTrue(optools.equiv_to_op(kde.core.uuobj, kde.uuobj))


if __name__ == '__main__':
  absltest.main()