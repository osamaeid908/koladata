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

"""Koda functions for creating schemas."""

import dataclasses
import types as py_types
import typing
from typing import Any
import warnings

from koladata.types import data_bag
from koladata.types import data_item as _  # pylint: disable=unused-import
from koladata.types import data_slice
from koladata.types import schema_constants


bag = data_bag.DataBag.empty


def new_schema(
    db: data_bag.DataBag | None = None, **attrs: data_slice.DataSlice
) -> data_slice.DataSlice:
  """Creates new schema in the given DataBag.

  Args:
    db: optional DataBag where the schema is created. If not provided, a new
      Databag is created.
    **attrs: attrs to set on the schema. Must be schemas.

  Returns:
    data_slice.DataSlice with the given attrs and kd.SCHEMA schema.
  """
  if db is None:
    db = bag()
  return db.new_schema(**attrs)


def list_schema(
    item_schema: data_slice.DataSlice, db: data_bag.DataBag | None = None
) -> data_slice.DataSlice:
  """Creates a list schema in the given DataBag.

  Args:
    item_schema: schema of the items in the list.
    db: optional DataBag where the schema is created. If not provided, a new
      Databag is created.

  Returns:
    data_slice.DataSlice representing a list schema.
  """
  if db is None:
    db = bag()
  return db.list_schema(item_schema)


def dict_schema(
    key_schema: data_slice.DataSlice,
    value_schema: data_slice.DataSlice,
    db: data_bag.DataBag | None = None,
) -> data_slice.DataSlice:
  """Creates a dict schema in the given DataBag.

  Args:
    key_schema: schema of the keys in the list.
    value_schema: schema of the values in the list.
    db: optional DataBag where the schema is created. If not provided, a new
      Databag is created.

  Returns:
    data_slice.DataSlice representing a dict schema.
  """
  if db is None:
    db = bag()
  return db.dict_schema(key_schema, value_schema)


def uu_schema(
    seed: str | None = None,
    *,
    db: data_bag.DataBag | None = None,
    **attrs: data_slice.DataSlice
) -> data_slice.DataSlice:
  """Creates a uu_schema in the given DataBag.

  Args:
    seed: optional string to seed the uuid computation with.
    db: optional DataBag where the schema is created. If not provided, a new
      Databag is created.
    **attrs: attrs to set on the schema. Must be schemas.

  Returns:
    data_slice.DataSlice with the given attrs and kd.SCHEMA schema.
  """
  if db is None:
    db = bag()
  return db.uu_schema(seed=seed, **attrs)


def named_schema(
    name: str | data_slice.DataSlice,
    *,
    db: data_bag.DataBag | None = None,
) -> data_slice.DataSlice:
  """Creates a named entity schema in the given DataBag.

  A named schema will have its item id derived only from its name, which means
  that two named schemas with the same name will have the same ItemId, even in
  different DataBags.

  Note that unlike other schema factories, this method does not take any attrs
  to avoid confisuion with the behavior of uu_schema. Please use
  named_schema(name).with_attrs(attrs) to create a named schema with attrs.

  Currently the named schema does not put any triples into the provided
  DataBag, but that might change in the future. For example, we might want to
  store the schema name in the DataBag for printing.

  Args:
    name: The name to use to derive the item id of the schema.
    db: optional DataBag where the schema is created. If not provided, a new
      Databag is created.

  Returns:
    data_slice.DataSlice with the ItemId of the required schema and kd.SCHEMA
    schema.
  """
  if db is None:
    db = bag()
  return db.named_schema(name)


def schema_from_py(tpe: type[Any]) -> data_slice.DataSlice:
  """Creates a Koda entity schema corresponding to the given Python type.

  This method supports the following Python types / type annotations
  recursively:
  - Primitive types: int, float, bool, str, bytes.
  - Collections: list[...], dict[...].
  - Unions: only "smth | None" or "Optional[smth]" is supported.
  - Dataclasses.

  This can be used in conjunction with kd.from_py to convert lists of Python
  objects to efficient Koda DataSlices. Because of the 'efficient' goal, we
  create an entity schema and do not use kd.OBJECT inside, which also results
  in strict type checking. If you do not care
  about efficiency or type safety, you can use kd.from_py(..., schema=kd.OBJECT)
  directly.

  Args:
    tpe: The Python type to create a schema for.

  Returns:
    A Koda entity schema corresponding to the given Python type. The returned
    schema is a uu-schema, in other words we always return the same output for
    the same input. For dataclasses, we use the module name and the class name
    to derive the itemid for the uu-schema.
  """

  def schema_from_py_impl(
      tpe: type[Any], db: data_bag.DataBag
  ) -> data_slice.DataSlice:
    if origin_tpe := typing.get_origin(tpe):
      if isinstance(origin_tpe, type) and issubclass(origin_tpe, list):
        (item_tpe,) = typing.get_args(tpe)
        return db.list_schema(schema_from_py_impl(item_tpe, db))
      if isinstance(origin_tpe, type) and issubclass(origin_tpe, dict):
        key_tpe, value_tpe = typing.get_args(tpe)
        return db.dict_schema(
            schema_from_py_impl(key_tpe, db), schema_from_py_impl(value_tpe, db)
        )
      if origin_tpe == py_types.UnionType or origin_tpe == typing.Union:
        options = typing.get_args(tpe)
        if len(options) != 2 or (options[1] != py_types.NoneType):
          raise TypeError(
              f'unsupported union type: {tpe}. kd.schema_from_py only'
              ' supports "smth | None" or "Optional[smth]".'
          )
        return schema_from_py_impl(options[0], db)
      raise TypeError(
          f'unsupported generic field type in kd.schema_from_py: {origin_tpe}.'
      )
    if not isinstance(tpe, type):
      raise TypeError(f'kd.schema_from_py expects a Python type, got {tpe}.')
    if dataclasses.is_dataclass(tpe):
      s = db.named_schema(
          f'__schema_from_py__{tpe.__module__}.{tpe.__qualname__}'
      )
      s.set_attrs(**{
          field.name: schema_from_py_impl(field.type, db)
          for field in dataclasses.fields(tpe)
      })
      return s
    if tpe == str:
      return schema_constants.STRING
    if tpe == bytes:
      return schema_constants.BYTES
    if tpe == int:
      # kd.from_py can return either INT32 or INT64 for integers, so we return
      # INT64 to be on the safe side.
      return schema_constants.INT64
    if tpe == float:
      # kd.from_py always returns FLOAT32 for floats, so we do the same for
      # consistency.
      return schema_constants.FLOAT32
    if tpe == bool:
      return schema_constants.BOOLEAN
    raise TypeError(f'unsupported type in kd.schema_from_py: {tpe}.')

  return schema_from_py_impl(tpe, bag())


def schema_from_py_type(tpe: type[Any]) -> data_slice.DataSlice:
  """A deprecated alias for kd.schema.schema_from_py."""
  warnings.warn(
      'kd.schema_from_py_type is deprecated. Use kd.schema_from_py instead.',
      RuntimeWarning,
  )
  return schema_from_py(tpe)
