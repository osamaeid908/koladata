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

import re

from absl.testing import absltest
from absl.testing import parameterized
from arolla import arolla
from koladata.expr import expr_eval
from koladata.expr import input_container
from koladata.expr import view
from koladata.operators import kde_operators
from koladata.testing import testing
from koladata.types import data_bag
from koladata.types import data_slice
from koladata.types import jagged_shape
from koladata.types import qtypes

I = input_container.InputContainer('I')
kde = kde_operators.kde
bag = data_bag.DataBag.empty
ds = data_slice.DataSlice.from_vals
DATA_SLICE = qtypes.DATA_SLICE
JAGGED_SHAPE = qtypes.JAGGED_SHAPE
NON_DETERMINISTIC_TOKEN = qtypes.NON_DETERMINISTIC_TOKEN
present = ds(arolla.present())


def generate_qtypes():
  for low_arg in [DATA_SLICE, arolla.UNSPECIFIED]:
    for high_arg in [DATA_SLICE, arolla.UNSPECIFIED]:
      for seed_arg in [DATA_SLICE, arolla.UNSPECIFIED]:
        yield JAGGED_SHAPE, low_arg, high_arg, seed_arg, NON_DETERMINISTIC_TOKEN, DATA_SLICE


QTYPES = frozenset(generate_qtypes())


class RandomRandintShapedTest(parameterized.TestCase):

  def assert_shape_and_range(self, res, shape, low, high):
    testing.assert_equal(expr_eval.eval(kde.shapes.get_shape(res)), shape)
    # Test all result items are present and within the range[low, high).
    testing.assert_equal(expr_eval.eval(kde.all(res >= low)), present)
    testing.assert_equal(expr_eval.eval(kde.all(res < high)), present)

  @parameterized.parameters(
      (jagged_shape.create_shape(),),
      (jagged_shape.create_shape([100]),),
      (jagged_shape.create_shape([2], [100, 200]),),
  )
  def test_eval(self, shape):
    low = -100
    high = 500

    with self.subTest('without low and high'):
      res = expr_eval.eval(kde.random.randint_shaped(shape))
      self.assert_shape_and_range(res, shape, 0, 2**63 - 1)

    with self.subTest('with high'):
      # Set high as positional argument
      res = expr_eval.eval(kde.random.randint_shaped(shape, high))
      self.assert_shape_and_range(res, shape, 0, high)

      # Set high as keyword argument
      res = expr_eval.eval(kde.random.randint_shaped(shape, high=high))
      self.assert_shape_and_range(res, shape, 0, high)

    with self.subTest('with low and high'):
      res = expr_eval.eval(kde.random.randint_shaped(shape, low, high))
      self.assert_shape_and_range(res, shape, low, high)

  def test_eval_with_seed(self):
    shape = jagged_shape.create_shape([2], [100, 200])
    res1 = expr_eval.eval(kde.random.randint_shaped(shape, seed=123))
    res2 = expr_eval.eval(kde.random.randint_shaped(shape, seed=123))
    res3 = expr_eval.eval(kde.random.randint_shaped(shape, seed=456))
    testing.assert_equal(res1, res2)
    self.assertNotEqual(res1.fingerprint, res3.fingerprint)

  def test_eval_without_seed(self):
    x = ds([[1, 2], [3]])
    expr = kde.random.randint_shaped_as(x)
    res1 = expr_eval.eval(expr)
    res2 = expr_eval.eval(expr)
    res3 = expr_eval.eval(kde.random.randint_shaped_as(x))
    self.assertNotEqual(res1.fingerprint, res2.fingerprint)
    self.assertNotEqual(res1.fingerprint, res3.fingerprint)

  def test_wrong_type(self):
    shape = jagged_shape.create_shape([2], [2, 1])

    with self.assertRaisesRegex(
        ValueError, re.escape('unsupported narrowing cast to INT64')
    ):
      _ = expr_eval.eval(kde.random.randint_shaped(shape, 0.5))

    with self.assertRaisesRegex(
        ValueError, re.escape('unsupported narrowing cast to INT64')
    ):
      _ = expr_eval.eval(kde.random.randint_shaped(shape, 5, 10.5))

    with self.assertRaisesRegex(
        ValueError, re.escape('unsupported narrowing cast to INT64')
    ):
      _ = expr_eval.eval(kde.random.randint_shaped(shape, seed=10.5))

  def test_invalid_range(self):
    shape = jagged_shape.create_shape([2], [2, 1])

    with self.assertRaisesRegex(
        ValueError, re.escape('low=10 must be less than high=10')
    ):
      _ = expr_eval.eval(kde.random.randint_shaped(shape, 10, 10))

    with self.assertRaisesRegex(
        ValueError, re.escape('low=20 must be less than high=10')
    ):
      _ = expr_eval.eval(kde.random.randint_shaped(shape, 20, 10))

  def test_qtype_signatures(self):
    # Limit the allowed qtypes and a random QType to speed up the test.
    self.assertCountEqual(
        arolla.testing.detect_qtype_signatures(
            kde.random.randint_shaped,
            possible_qtypes=(
                arolla.UNSPECIFIED,
                DATA_SLICE,
                JAGGED_SHAPE,
                NON_DETERMINISTIC_TOKEN,
            ),
        ),
        QTYPES,
    )

  def test_view(self):
    self.assertTrue(
        view.has_koda_view(
            kde.random.randint_shaped(I.shape, I.low, I.high, I.seed)
        )
    )


if __name__ == '__main__':
  absltest.main()
