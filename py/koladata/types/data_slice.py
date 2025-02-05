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

"""DataSlice abstraction."""

import dataclasses
import functools
from typing import Any
import warnings

from arolla import arolla
from koladata.expr import py_expr_eval_py_ext as _py_expr_eval_py_ext
from koladata.types import data_bag_py_ext as _data_bag_py_ext
from koladata.types import data_slice_py_ext as _data_slice_py_ext
from koladata.types import general_eager_ops
from koladata.types import jagged_shape


DataBag = _data_bag_py_ext.DataBag
DataSlice = _data_slice_py_ext.DataSlice

_eval_op = _py_expr_eval_py_ext.eval_op


def _add_method(cls, method_name: str):
  """Returns a callable / decorator to put it as a cls's method.

  Adds methods only to the class it was called on (then it is also available in
  cls' subclasses.

  Example on how to define methods:

    @DataSlice._add_method('flatten')
    def _flatten(self, from, to) -> DataSlice:
      return _eval_op('kde.flatten', self, from, to)

  Args:
    cls: DataSlice or its subclass.
    method_name: wraps the Python function as a method with `method_name`.
  """

  # TODO: Find a way to have a sensible docstring that DataSlice
  # method should have (e.g. exclude first argument as it is `self`).
  def wrap(op):
    """Wraps eager `op` into a DataSlice method `method_name`."""
    cls.internal_register_reserved_class_method_name(method_name)
    setattr(cls, method_name, op)
    return op

  return wrap


def register_reserved_class_method_names(cls):
  """Decorates `cls` such that it registers the names of all its methods.

  A convenience decorator, so that statements such as:

  `cls.internal_register_reserved_class_method_name('some_method')`

  do not need to be written explicitly.

  Example:

    @DataSlice.subclass
    class SubDataSlice(DataSlice):

      def some_method(self):
        ...

    # Causes `some_method` to be a forbiden Koda attribute that cannot be
    # accessed through `__getattr__` or `__setattr__`, but `get_attr` and
    # `set_attr` methods need to be used.

  Args:
    cls: DataSlice subclass

  Returns:
    cls
  """
  assert issubclass(cls, DataSlice)
  for attr_name in dir(cls):
    if not attr_name.startswith('_') and callable(getattr(cls, attr_name)):
      cls.internal_register_reserved_class_method_name(attr_name)
  return cls


# IPython/Colab try to access those attributes on any object and expect
# particular behavior from them, so we disallow accessing such attributes
# on a DataSlice via __getattr__.
RESERVED_ATTRIBUTES_WITHOUT_LEADING_UNDERSCORE = frozenset([
    'add_method',
    'getdoc',
    'trait_names',
])


def _init_data_slice_class():
  """Initializes DataSlice class."""
  if hasattr(DataSlice, 'add_method'):
    return
  # NOTE: `add_method` is available in subclasses too.
  DataSlice._add_method = classmethod(_add_method)  # pylint: disable=protected-access
  for name in RESERVED_ATTRIBUTES_WITHOUT_LEADING_UNDERSCORE:
    DataSlice.internal_register_reserved_class_method_name(name)


### Implementation of the DataSlice's additional functionality.


_init_data_slice_class()


##### DataSlice methods. #####

# TODO: Remove these aliases.
DataSlice._add_method('with_db')(DataSlice.with_bag)  # pylint: disable=protected-access
DataSlice._add_method('no_db')(DataSlice.no_bag)  # pylint: disable=protected-access


@DataSlice._add_method('__dir__')  # pylint: disable=protected-access
def _dir(self) -> list[str]:
  """Returns the list of attrs accessible through `getattr(slice, my_attr)`."""
  attrs = []
  try:
    attrs = self.get_attr_names(intersection=True)
  except ValueError:
    pass
  # We only include those attributes that can be called through `slice.my_attr`.
  attrs = {attr for attr in attrs if self.internal_is_compliant_attr_name(attr)}
  methods = set(super(DataSlice, self).__dir__())
  return sorted(methods | attrs)


@DataSlice._add_method('maybe')  # pylint: disable=protected-access
def _maybe(self, attr_name: str) -> DataSlice:
  # NOTE: Calling `get_attr`, instead of _eval_op, because it is implemented
  # in C Python.
  return self.get_attr(attr_name, None)


@DataSlice._add_method('has_attr')  # pylint: disable=protected-access
def _has_attr(self, attr_name: str) -> DataSlice:
  return _eval_op('kde.has_attr', self, attr_name)


@DataSlice._add_method('reshape')  # pylint: disable=protected-access
def _reshape(self, shape: jagged_shape.JaggedShape) -> DataSlice:
  if not isinstance(shape, jagged_shape.JaggedShape):
    raise TypeError(f'`shape` must be a JaggedShape, got: {shape}')
  return _eval_op('kde.reshape', self, shape)


@DataSlice._add_method('reshape_as')  # pylint: disable=protected-access
def _reshape_as(self, shape_from: DataSlice) -> DataSlice:
  if not isinstance(shape_from, DataSlice):
    raise TypeError(f'`shape_from` must be a DataSlice, got: {shape_from}')
  return _eval_op('kde.reshape_as', self, shape_from)


@DataSlice._add_method('flatten')  # pylint: disable=protected-access
def _flatten(
    self,
    from_dim: int | DataSlice = DataSlice.from_vals(arolla.int64(0)),
    to_dim: Any = arolla.unspecified(),
) -> DataSlice:
  return _eval_op('kde.flatten', self, from_dim, to_dim)


@DataSlice._add_method('add_dim')  # pylint: disable=protected-access
def _add_dim(self, sizes: Any) -> DataSlice:
  return _eval_op('kde.add_dim', self, sizes)


@DataSlice._add_method('repeat')  # pylint: disable=protected-access
def _repeat(self, sizes: Any) -> DataSlice:
  return _eval_op('kde.repeat', self, sizes)


@DataSlice._add_method('select')  # pylint: disable=protected-access
def _select(
    self, fltr: Any, expand_filter: bool | DataSlice = DataSlice.from_vals(True)
) -> DataSlice:
  return _eval_op('kde.select', self, fltr, expand_filter=expand_filter)


@DataSlice._add_method('select_present')  # pylint: disable=protected-access
def _select_present(self) -> DataSlice:
  return _eval_op('kde.select_present', self)


@DataSlice._add_method('select_items')  # pylint: disable=protected-access
def _select_items(self, fltr: Any) -> DataSlice:
  return _eval_op('kde.select_items', self, fltr)


@DataSlice._add_method('select_keys')  # pylint: disable=protected-access
def _select_keys(self, fltr: Any) -> DataSlice:
  return _eval_op('kde.select_keys', self, fltr)


@DataSlice._add_method('select_values')  # pylint: disable=protected-access
def _select_values(self, fltr: Any) -> DataSlice:
  return _eval_op('kde.select_values', self, fltr)


@DataSlice._add_method('expand_to')  # pylint: disable=protected-access
def _expand_to(
    self, target: Any, ndim: Any = arolla.unspecified()
) -> DataSlice:
  return _eval_op('kde.expand_to', self, target, ndim)


@DataSlice._add_method('list_size')  # pylint: disable=protected-access
def _list_size(self) -> DataSlice:
  return _eval_op('kde.list_size', self)


@DataSlice._add_method('dict_size')  # pylint: disable=protected-access
def _dict_size(self) -> DataSlice:
  return _eval_op('kde.dict_size', self)


@DataSlice._add_method('dict_update')  # pylint: disable=protected-access
def _dict_update(
    self, keys: Any, values: Any = arolla.unspecified()
) -> DataBag:
  return _eval_op('kde.dict_update', self, keys=keys, values=values)


@DataSlice._add_method('with_dict_update')  # pylint: disable=protected-access
def _with_dict_update(
    self, keys: Any, values: Any = arolla.unspecified()
) -> DataSlice:
  return _eval_op('kde.with_dict_update', self, keys=keys, values=values)


@DataSlice._add_method('follow')  # pylint: disable=protected-access
def _follow(self) -> DataSlice:
  return _eval_op('kde.follow', self)


@DataSlice._add_method('extract')  # pylint: disable=protected-access
def _extract(self, schema: Any = arolla.unspecified()) -> DataSlice:
  return _eval_op('kde.extract', self, schema)


@DataSlice._add_method('extract_bag')  # pylint: disable=protected-access
def _extract_bag(self, schema: Any = arolla.unspecified()) -> DataBag:
  return _eval_op('kde.extract_bag', self, schema)


@DataSlice._add_method('clone')  # pylint: disable=protected-access
def _clone(
    self,
    *,
    itemid: Any = arolla.unspecified(),
    schema: Any = arolla.unspecified(),
    **overrides: Any,
) -> DataSlice:
  return _py_expr_eval_py_ext.eval_op(
      'kde.clone', self, itemid=itemid, schema=schema, **overrides
  )


@DataSlice._add_method('shallow_clone')  # pylint: disable=protected-access
def _shallow_clone(
    self,
    *,
    itemid: Any = arolla.unspecified(),
    schema: Any = arolla.unspecified(),
    **overrides: Any,
) -> DataSlice:
  return _py_expr_eval_py_ext.eval_op(
      'kde.shallow_clone', self, itemid=itemid, schema=schema, **overrides
  )


@DataSlice._add_method('deep_clone')  # pylint: disable=protected-access
def _deep_clone(
    self, schema: Any = arolla.unspecified(), **overrides: Any
) -> DataSlice:
  return _py_expr_eval_py_ext.eval_op(
      'kde.deep_clone', self, schema, **overrides
  )


@DataSlice._add_method('deep_uuid')  # pylint: disable=protected-access
def _deep_uuid(
    self,
    schema: Any = arolla.unspecified(),
    *,
    seed: str | DataSlice = DataSlice.from_vals(''),
) -> DataSlice:
  return _eval_op('kde.deep_uuid', self, schema, seed=seed)


@DataSlice._add_method('fork_bag')  # pylint: disable=protected-access
# TODO: Remove this alias.
@DataSlice._add_method('fork_db')  # pylint: disable=protected-access
def _fork_bag(self) -> DataSlice:
  if self.get_bag() is None:
    raise ValueError(
        'fork_bag expects the DataSlice to have a DataBag attached'
    )
  return self.with_bag(self.get_bag().fork(mutable=True))


@DataSlice._add_method('with_merged_bag')  # pylint: disable=protected-access
def _with_merged_bag(self) -> DataSlice:
  return _eval_op('kde.with_merged_bag', self)


@DataSlice._add_method('enriched')  # pylint: disable=protected-access
def _enriched(self, *bag: DataBag) -> DataSlice:
  return _eval_op('kde.enriched', self, *bag)


@DataSlice._add_method('updated')  # pylint: disable=protected-access
def _updated(self, *bag: DataBag) -> DataSlice:
  return _eval_op('kde.updated', self, *bag)


@DataSlice._add_method('__neg__')  # pylint: disable=protected-access
def _neg(self) -> DataSlice:
  return _eval_op('kde.math.neg', self)


@DataSlice._add_method('__pos__')  # pylint: disable=protected-access
def _pos(self) -> DataSlice:
  return _eval_op('kde.math.pos', self)


DataSlice._add_method('with_name')(general_eager_ops.with_name)  # pylint: disable=protected-access


@DataSlice._add_method('ref')  # pylint: disable=protected-access
def _ref(self) -> DataSlice:
  return _eval_op('kde.ref', self)


@DataSlice._add_method('get_itemid')  # pylint: disable=protected-access
def _get_itemid(self) -> DataSlice:
  return _eval_op('kde.get_itemid', self)


# TODO: Remove this alias.
@DataSlice._add_method('as_itemid')  # pylint: disable=protected-access
def _as_itemid(self) -> DataSlice:
  warnings.warn(
      'as_itemid is deprecated. Use get_itemid instead.',
      RuntimeWarning,
  )
  return self.get_itemid()


@DataSlice._add_method('get_dtype')  # pylint: disable=protected-access
def _get_dtype(self) -> DataSlice:
  return _eval_op('kde.get_dtype', self)


@DataSlice._add_method('get_obj_schema')  # pylint: disable=protected-access
def _get_obj_schema(self) -> DataSlice:
  return _eval_op('kde.get_obj_schema', self)


@DataSlice._add_method('with_schema_from_obj')  # pylint: disable=protected-access
def _with_schema_from_obj(self) -> DataSlice:
  return _eval_op('kde.with_schema_from_obj', self)


@DataSlice._add_method('get_ndim')  # pylint: disable=protected-access
def _get_ndim(self) -> DataSlice:
  return _eval_op('kde.get_ndim', self)


@DataSlice._add_method('get_present_count')  # pylint: disable=protected-access
def _get_present_count(self) -> DataSlice:
  return _eval_op('kde.count', self)


@DataSlice._add_method('get_size')  # pylint: disable=protected-access
def _get_size(self) -> DataSlice:
  return _eval_op('kde.size', self)


@DataSlice._add_method('is_primitive')  # pylint: disable=protected-access
def _is_primitive(self) -> DataSlice:
  return _eval_op('kde.is_primitive', self)


@DataSlice._add_method('get_item_schema')  # pylint: disable=protected-access
def _get_item_schema(self) -> DataSlice:
  return _eval_op('kde.get_item_schema', self)


@DataSlice._add_method('get_key_schema')  # pylint: disable=protected-access
def _get_key_schema(self) -> DataSlice:
  return _eval_op('kde.get_key_schema', self)


@DataSlice._add_method('get_value_schema')  # pylint: disable=protected-access
def _get_value_schema(self) -> DataSlice:
  return _eval_op('kde.get_value_schema', self)


@DataSlice._add_method('stub')  # pylint: disable=protected-access
def _stub(self, attrs: DataSlice = DataSlice.from_vals([])) -> DataSlice:
  return _eval_op('kde.stub', self, attrs=attrs)


@DataSlice._add_method('with_attrs')  # pylint: disable=protected-access
def _with_attrs(
    self,
    *,
    update_schema: bool | DataSlice = DataSlice.from_vals(False),
    **attrs,
) -> DataSlice:
  return _eval_op('kde.with_attrs', self, update_schema=update_schema, **attrs)


@DataSlice._add_method('with_attr')  # pylint: disable=protected-access
def _with_attr(
    self,
    attr_name: str | DataSlice,
    value: Any,
    update_schema: bool | DataSlice = DataSlice.from_vals(False),
) -> DataSlice:
  return _eval_op('kde.with_attr', self, attr_name, value, update_schema)


@DataSlice._add_method('take')  # pylint: disable=protected-access
def _take(self, indices: Any) -> DataSlice:
  return _eval_op('kde.take', self, indices)


@DataSlice._add_method('to_py')  # pylint: disable=protected-access
def to_py(
    ds: DataSlice,
    max_depth: int = 2,
    obj_as_dict: bool = False,
    include_missing_attrs: bool = True,
) -> Any:
  """Returns a readable python object from a DataSlice.

  Attributes, lists, and dicts are recursively converted to Python objects.

  Args:
    ds: A DataSlice
    max_depth: Maximum depth for recursive printing. Each attribute, list, and
      dict increments the depth by 1. Use -1 for unlimited depth.
    obj_as_dict: Whether to convert objects to python dicts. By default objects
      are converted to automatically constructed 'Obj' dataclass instances.
    include_missing_attrs: whether to include attributes with None value in
      objects.
  """
  if max_depth >= 0:
    max_depth += ds.get_ndim()

  orig_bag = ds.get_bag()
  db = DataBag.empty()
  ds = db.implode(ds.no_bag(), -1)
  if orig_bag is not None:
    ds = ds.enriched(orig_bag)
  return _to_py_impl(ds, {}, 0, max_depth, obj_as_dict, include_missing_attrs)


@DataSlice._add_method('to_pytree')  # pylint: disable=protected-access
def to_pytree(
    ds: DataSlice, max_depth: int = 2, include_missing_attrs: bool = True
) -> Any:
  """Returns a readable python object from a DataSlice.

  Attributes, lists, and dicts are recursively converted to Python objects.
  Objects are converted to Python dicts.

  Same as kd.to_py(..., obj_as_dict=True)

  Args:
    ds: A DataSlice
    max_depth: Maximum depth for recursive printing. Each attribute, list, and
      dict increments the depth by 1. Use -1 for unlimited depth.
    include_missing_attrs: whether to include attributes with None value in
      objects.
  """
  return to_py(
      ds,
      max_depth=max_depth,
      obj_as_dict=True,
      include_missing_attrs=include_missing_attrs,
  )


def _to_py_impl(
    ds: DataSlice,
    obj_id_to_python_obj: dict[DataSlice, Any],
    depth: int,
    max_depth: int,
    obj_as_dict: bool,
    include_missing_attrs: bool,
) -> Any:
  """Recursively converts a DataItem to a Python object."""
  assert ds.get_ndim() == 0

  existing = obj_id_to_python_obj.get(ds)
  if existing is not None:
    return existing

  if ds.is_empty():
    return None

  if ds.is_primitive():
    return ds.internal_as_py()

  if ds.get_bag() is None:
    return {}

  schema = ds.get_schema()

  # TODO: Move to_py() out of data_slice.py, remove
  # internal_is_any_schema, and use schema == ANY instead.
  if schema.internal_is_any_schema():
    raise ValueError(
        f'cannot convert a DataSlice with ANY schema to Python: {ds}'
    )

  # TODO: Move to_py() out of data_slice.py, remove
  # internal_is_itemid_schema, and use schema == ITEMID instead.
  if (
      max_depth >= 0 and depth >= max_depth
  ) or schema.internal_is_itemid_schema():
    return ds

  is_list = ds.is_list()
  is_dict = ds.is_dict()

  # Remove special attributes
  attr_names = sorted(
      set(ds.get_attr_names(intersection=True))
      - {'__items__', '__keys__', '__values__'}
  )
  assert not (attr_names and (is_list or is_dict))

  if attr_names and not obj_as_dict:
    obj_class = dataclasses.make_dataclass(
        'Obj',
        [
            (attr_name, Any, dataclasses.field(default=None))
            for attr_name in attr_names
        ],
        eq=False,
    )

    def eq(x, y):
      """Checks whether two dataclasses are equal ignoring types."""
      return dataclasses.is_dataclass(y) and dataclasses.asdict(
          x
      ) == dataclasses.asdict(y)

    obj_class.__eq__ = eq

    py_obj = obj_class()
  elif is_list:
    py_obj = []
  else:
    py_obj = {}

  obj_id_to_python_obj[ds] = py_obj

  attrs = {}
  next_depth = depth + 1
  for attr_name in attr_names:
    attr_ds = ds.get_attr(attr_name)
    attr_value = _to_py_impl(
        attr_ds,
        obj_id_to_python_obj,
        next_depth,
        max_depth,
        obj_as_dict,
        include_missing_attrs,
    )
    if include_missing_attrs or attr_value is not None:
      attrs[attr_name] = attr_value

  if dataclasses.is_dataclass(py_obj):
    for name, value in attrs.items():
      setattr(py_obj, name, value)
  elif attrs and obj_as_dict and not is_dict and not is_list:
    py_obj.update(attrs)  # pytype: disable=attribute-error

  if is_list:
    list_values = py_obj
    assert isinstance(list_values, list)
    for child_ds in ds:
      list_values.append(
          _to_py_impl(
              child_ds,
              obj_id_to_python_obj,
              next_depth,
              max_depth,
              obj_as_dict,
              include_missing_attrs,
          )
      )

  if is_dict:
    dict_values = py_obj
    for key in ds:
      value_ds = ds[key]
      dict_values[key.no_bag().internal_as_py()] = _to_py_impl(
          value_ds,
          obj_id_to_python_obj,
          next_depth,
          max_depth,
          obj_as_dict,
          include_missing_attrs,
      )

  return py_obj


##### DataSlice Magic methods. #####


@DataSlice._add_method('__add__')  # pylint: disable=protected-access
def _add(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.core.add', self, other)


@DataSlice._add_method('__radd__')  # pylint: disable=protected-access
def _radd(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.core.add', other, self)


@DataSlice._add_method('__sub__')  # pylint: disable=protected-access
def _sub(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.math.subtract', self, other)


@DataSlice._add_method('__rsub__')  # pylint: disable=protected-access
def _rsub(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.math.subtract', other, self)


@DataSlice._add_method('__mul__')  # pylint: disable=protected-access
def _mul(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.math.multiply', self, other)


@DataSlice._add_method('__rmul__')  # pylint: disable=protected-access
def _rmul(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.math.multiply', other, self)


@DataSlice._add_method('__truediv__')  # pylint: disable=protected-access
def _truediv(self, other):
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.math.divide', self, other)


@DataSlice._add_method('__rtruediv__')  # pylint: disable=protected-access
def _rtruediv(self, other):
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.math.divide', other, self)


@DataSlice._add_method('__floordiv__')  # pylint: disable=protected-access
def _floordiv(self, other):
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.math.floordiv', self, other)


@DataSlice._add_method('__rfloordiv__')  # pylint: disable=protected-access
def _rfloordiv(self, other):
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.math.floordiv', other, self)


@DataSlice._add_method('__mod__')  # pylint: disable=protected-access
def _mod(self, other):
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.math.mod', self, other)


@DataSlice._add_method('__rmod__')  # pylint: disable=protected-access
def _rmod(self, other):
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.math.mod', other, self)


@DataSlice._add_method('__pow__')  # pylint: disable=protected-access
def _pow(self, other):
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.math.pow', self, other)


@DataSlice._add_method('__rpow__')  # pylint: disable=protected-access
def _rpow(self, other):
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.math.pow', other, self)


@DataSlice._add_method('__and__')  # pylint: disable=protected-access
def _and(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.apply_mask', self, other)


@DataSlice._add_method('__rand__')  # pylint: disable=protected-access
def _rand(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.apply_mask', other, self)


@DataSlice._add_method('__eq__')  # pylint: disable=protected-access
def _eq(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.equal', self, other)


@DataSlice._add_method('__ne__')  # pylint: disable=protected-access
def _ne(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.not_equal', self, other)


@DataSlice._add_method('__gt__')  # pylint: disable=protected-access
def _gt(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.greater', self, other)


@DataSlice._add_method('__ge__')  # pylint: disable=protected-access
def _ge(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.greater_equal', self, other)


@DataSlice._add_method('__lt__')  # pylint: disable=protected-access
def _lt(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.less', self, other)


@DataSlice._add_method('__le__')  # pylint: disable=protected-access
def _le(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.less_equal', self, other)


@DataSlice._add_method('__or__')  # pylint: disable=protected-access
def _or(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.coalesce', self, other)


@DataSlice._add_method('__ror__')  # pylint: disable=protected-access
def _ror(self, other: Any) -> DataSlice:
  if isinstance(other, arolla.Expr):
    return NotImplemented
  return _eval_op('kde.coalesce', other, self)


@DataSlice._add_method('__invert__')  # pylint: disable=protected-access
def _invert(self) -> DataSlice:
  return _eval_op('kde.has_not', self)


class SlicingHelper:
  """Slicing helper for DataSlice.

  It is a syntactic sugar for kd.subslice. That is, kd.subslice(ds, *slices)
  is equivalent to ds.S[*slices]. For example,
    kd.subslice(x, 0) == x.S[0]
    kd.subslice(x, 0, 1, kd.item(0)) == x.S[0, 1, kd.item(0)]
    kd.subslice(x, slice(0, -1)) == x.S[0:-1]
    kd.subslice(x, slice(0, -1), slice(0, 1), slice(1, None))
      == x.S[0:-1, 0:1, 1:]
    kd.subslice(x, ..., slice(1, None)) == x.S[..., 1:]
    kd.subslice(x, slice(1, None)) == x.S[1:]

  Please see kd.subslice for more detailed explanations and examples.
  """

  def __init__(self, ds: DataSlice):
    self._ds = ds

  def __getitem__(self, s):
    slices = s if isinstance(s, tuple) else [s]
    return _eval_op('kde.subslice', self._ds, *slices)


class ListSlicingHelper:
  """ListSlicing helper for DataSlice.

  x.L on DataSlice returns a ListSlicingHelper, which treats the first dimension
  of DataSlice x as a a list.
  """

  def __init__(self, ds: DataSlice):
    ndim = ds.get_ndim().internal_as_py()
    if not ndim:
      raise ValueError('DataSlice must have at least one dimension to iterate.')
    self._ds = ds
    self._ndim = ndim

  @functools.cached_property
  def _imploded_ds(self) -> DataSlice:
    return DataBag.empty().implode(self._ds.no_bag(), -1)

  def __getitem__(self, s):
    return _eval_op(
        'kde.explode', self._imploded_ds[s], self._ndim - 1
    ).with_bag(self._ds.get_bag())

  def __len__(self) -> int:
    return _eval_op(
        'kde.shapes.dim_sizes', _eval_op('kde.get_shape', self._ds), 0
    ).internal_as_py()[0]

  def __iter__(self):
    return (self[i] for i in range(len(self)))


# NOTE: we can create a decorator for adding property similar to add_method, if
# we need it for more properties.
DataSlice.internal_register_reserved_class_method_name('S')
DataSlice.S = property(SlicingHelper)
DataSlice.internal_register_reserved_class_method_name('L')
DataSlice.L = property(ListSlicingHelper)


@functools.cache
def unspecified():
  return DataSlice._unspecified()  # pylint: disable=protected-access
