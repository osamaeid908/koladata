// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#ifndef KOLADATA_DATA_SLICE_H_
#define KOLADATA_DATA_SLICE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include "absl/base/nullability.h"
#include "absl/container/btree_set.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "koladata/data_bag.h"
#include "koladata/internal/data_item.h"
#include "koladata/internal/data_slice.h"
#include "koladata/internal/dtype.h"
#include "arolla/jagged_shape/dense_array/jagged_shape.h"
#include "arolla/qtype/qtype.h"
#include "arolla/util/refcount_ptr.h"
#include "arolla/util/text.h"

namespace koladata {

constexpr absl::string_view kDataSliceQValueSpecializationKey =
    "::koladata::python::DataSlice";

constexpr absl::string_view kDataItemQValueSpecializationKey =
    "::koladata::python::DataItem";

constexpr absl::string_view kListItemQValueSpecializationKey =
    "::koladata::python::ListItem";

constexpr absl::string_view kDictItemQValueSpecializationKey =
    "::koladata::python::DictItem";

constexpr absl::string_view kSchemaItemQValueSpecializationKey =
    "::koladata::python::SchemaItem";

// This abstraction implements the API of all public DataSlice functionality
// users can access. It is used as the main entry point to business logic
// implementation and all the processing is delegated to it from C Python
// bindings for DataSlice.
//
// C Python bindings for DataSlice is processing only the minimum part necessary
// to extract information from PyObject(s) and propagate it to appropriate
// methods of this class and DataBag class.
class DataSlice {
 public:
  using JaggedShape = arolla::JaggedDenseArrayShape;
  using JaggedShapePtr = arolla::JaggedDenseArrayShapePtr;

  // Creates a DataSlice with necessary invariant checks:
  // * shape must be compatible with the size of DataSliceImpl;
  // * schema must be consistent with the contents.
  //
  // Callers must ensure that schema will be compatible with passed data. If the
  // caller does not handle schema itself, it should rely on
  // DataSlice::WithSchema, instead.
  static absl::StatusOr<DataSlice> Create(
      internal::DataSliceImpl impl, JaggedShapePtr shape,
      internal::DataItem schema, std::shared_ptr<DataBag> db = nullptr);

  // Same as above, but creates a DataSlice from DataItem. Shape is created
  // implicitly with rank == 0.
  //
  // Callers must ensure that schema will be compatible with passed data. If the
  // caller does not handle schema itself, it should rely on
  // DataSlice::WithSchema, instead.
  static absl::StatusOr<DataSlice> Create(
      const internal::DataItem& item, internal::DataItem schema,
      std::shared_ptr<DataBag> db = nullptr);

  // Creates a DataSlice with schema built from data's dtype. Supported only for
  // primitive DTypes.
  static absl::StatusOr<DataSlice> CreateWithSchemaFromData(
      internal::DataSliceImpl impl, JaggedShapePtr shape,
      std::shared_ptr<DataBag> db = nullptr);

  // Convenience factory method that accepts JaggedShapePtr, so that we can use
  // implementation-agnostic constructions in visitors passed to VisitImpl.
  static absl::StatusOr<DataSlice> Create(
      const internal::DataItem& item, JaggedShapePtr shape,
      internal::DataItem schema, std::shared_ptr<DataBag> db = nullptr);

  // Convenience factory method that creates a DataSlice from StatusOr. Returns
  // the same error in case of error.
  static absl::StatusOr<DataSlice> Create(
      absl::StatusOr<internal::DataSliceImpl> slice_or, JaggedShapePtr shape,
      internal::DataItem schema, std::shared_ptr<DataBag> db = nullptr);

  // Convenience factory method that creates a DataSlice from StatusOr. Returns
  // the same error in case of error.
  static absl::StatusOr<DataSlice> Create(
      absl::StatusOr<internal::DataItem> item_or, JaggedShapePtr shape,
      internal::DataItem schema, std::shared_ptr<DataBag> db = nullptr);

  // Default-constructed DataSlice is a single missing item with scalar shape
  // and unknown dtype.
  DataSlice() : internal_(arolla::RefcountPtr<Internal>::Make()) {};

  // Returns a JaggedShapePtr of this slice.
  const JaggedShapePtr& GetShapePtr() const { return internal_->shape_; }

  // Returns a JaggedShape of this slice.
  const JaggedShape& GetShape() const {
    DCHECK_NE(internal_->shape_, nullptr);
    return *internal_->shape_;
  }

  // Returns a new DataSlice whose values and shape are broadcasted to `shape`.
  // In case DataSlice cannot be broadcasted to `shape`, appropriate Status
  // error is returned.
  absl::StatusOr<DataSlice> BroadcastToShape(JaggedShapePtr shape) const;

  // Returns a new DataSlice with the same values and a new `shape`. Returns an
  // error if the shape is not compatible with the existing shape.
  absl::StatusOr<DataSlice> Reshape(JaggedShapePtr shape) const;

  // Returns a DataSlice that represents a Schema.
  DataSlice GetSchema() const;

  // Returns a DataItem holding a schema.
  const internal::DataItem& GetSchemaImpl() const { return internal_->schema_; }

  // Returns a new DataSlice with the updated `schema`. In case `schema` cannot
  // be assigned to this DataSlice, the appropriate Error is returned.
  // DataSlice's schema cannot take `schema` as a new value for various reasons,
  // e.g. schema points to objects, but the contents are primitives, etc.
  absl::StatusOr<DataSlice> WithSchema(const DataSlice& schema) const;

  // Returns OkStatus if this DataSlice represents a Schema. In particular, it
  // means that .item() can be safely called.
  absl::Status VerifyIsSchema() const;

  // Returns OkStatus if this DataSlice represents a primitive Schema.
  absl::Status VerifyIsPrimitiveSchema() const;

  // Returns an original schema from NoFollow slice. If this slice is not
  // NoFollow, an error is returned.
  absl::StatusOr<DataSlice> GetNoFollowedSchema() const;

  // Returns a reference to a DataBag that this DataSlice has a reference to.
  const absl::Nullable<std::shared_ptr<DataBag>>& GetDb() const {
    return internal_->db_;
  }

  // Returns a new DataSlice with a new reference to DataBag `db`.
  DataSlice WithDb(std::shared_ptr<DataBag> db) const {
    return DataSlice(internal_->impl_, GetShapePtr(), GetSchemaImpl(), db);
  }

  // Returns true iff `other` represents the same DataSlice with same data
  // contents as well as members (db, schema, shape).
  bool IsEquivalentTo(const DataSlice& other) const;

  // Returns all attribute names that are defined on this DataSlice. In case of
  // OBJECT schema, attribute names are fetched from `__schema__` attribute.
  absl::StatusOr<absl::btree_set<arolla::Text>> GetAttrNames() const;

  // Returns a new DataSlice with a reference to the same DataBag if it exists
  // as an attribute `attr_name` of this Object. Returns a status error on
  // missing or invalid attribute requests.
  absl::StatusOr<DataSlice> GetAttr(absl::string_view attr_name) const;

  // Returns a new DataSlice with a reference to the same DataBag. Missing
  // values are filled with `default_value`. This also allows fetching an
  // attribute that does not exist. Returns an error in case of missing DataBag.
  absl::StatusOr<DataSlice> GetAttrWithDefault(
      absl::string_view attr_name, const DataSlice& default_value) const;

  // Sets an attribute `attr_name` of this object to `values`. Possible only if
  // it contains a reference to a DataBag.
  absl::Status SetAttr(absl::string_view attr_name,
                       const DataSlice& values) const;

  // Sets an attribute `attr_name` of this object to `values`. Also updates
  // schema with `values` schema. In case of object-level schema, attribute
  // "__schema__"'s schema is updated. Possible only if it contains a reference
  // to a DataBag.
  absl::Status SetAttrWithUpdateSchema(absl::string_view attr_name,
                                       const DataSlice& values) const;

  // Removes an attribute `attr_name` of this object. Entity Schema is not
  // updated, while Object Schema is. If attribute is being deleted on Schema
  // itself, Entity schema is updated. Returns error if attribute does not exist
  // on the schema.
  absl::Status DelAttr(absl::string_view attr_name) const;

  // Returns true if the slice contains ObjectIds and the first present ObjectId
  // is a list. Used to choose whether to apply list or dict operation.
  bool IsFirstPresentAList() const;

  // Gets a value from each dict in this slice (it must be slice of dicts) using
  // the corresponding keys (the shape of `keys` must be compatible with shape
  // if the dicts slice) and returns them as a DataSlice of the same size.
  absl::StatusOr<DataSlice> GetFromDict(const DataSlice& keys) const;

  // Sets one value in every dict in this slice. The slice must be slice of
  // dicts. `keys` and `values` must be compatible with shape of the dicts slice
  // and broadcastable to one another.
  absl::Status SetInDict(const DataSlice& keys, const DataSlice& values) const;

  // Returns all keys of all dicts in this slice (it must be slice of dicts).
  // Shape of the output slice has an additional dimension.
  absl::StatusOr<DataSlice> GetDictKeys() const;

  // Gets a value from each list in this slice (it must be slice of lists) using
  // the corresponding indices (the shape of `indices` must be compatible with
  // shape if the lists slice) and returns them as a DataSlice of the same size.
  absl::StatusOr<DataSlice> GetFromList(const DataSlice& indices) const;

  // Same as GetFromList, but also removes the values from the lists.
  absl::StatusOr<DataSlice> PopFromList(const DataSlice& indices) const;

  // Removes and returns the last value in each list.
  absl::StatusOr<DataSlice> PopFromList() const;

  // Sets one value in every list in this slice. The slice must be slice of
  // lists. `indices` and `values` must be compatible with shape of the lists
  // slice and broadcastable to one another.
  absl::Status SetInList(const DataSlice& indices,
                         const DataSlice& values) const;

  // Append one value to each list. The slice must be slice of
  // lists. `values` must be compatible with shape of the lists slice.
  absl::Status AppendToList(const DataSlice& values) const;

  // Clear all dicts or lists. The slice must contain either only lists or only
  // dicts.
  absl::Status ClearDictOrList() const;

  // Gets [start, stop) range from each list and returns as a data slice with an
  // additional dimension.
  absl::StatusOr<DataSlice> ExplodeList(int64_t start,
                                        std::optional<int64_t> stop) const;

  // Replaces [start, stop) range in each list with given values.
  absl::Status ReplaceInList(int64_t start, std::optional<int64_t> stop,
                             const DataSlice& values) const;

  // Removes [start, stop) range in each list.
  absl::Status RemoveInList(int64_t start, std::optional<int64_t> stop) const;

  // Removes a value with given index in each list.
  absl::Status RemoveInList(const DataSlice& indices) const;

  // Returns a DataSlice with OBJECT schema.
  // * For primitives no data change is done.
  // * For Entities schema is stored as '__schema__' attribute.
  // * Embedding Entities requires a DataSlice to be associated with a DataBag.
  // * If `overwrite` is true '__schema__' attribute is overwritten. Otherwise,
  //   an error is returned on conflict.
  absl::StatusOr<DataSlice> EmbedSchema(bool overwrite = true) const;

  // Call `visitor` with the present implementation type (DataItem or
  // DataSliceImpl). `visitor` should handle both cases when underlying
  // implementation is DataSliceImpl and when it is DataItem. Ideally, your
  // `visitor` will use implementation agnostic functionality for better
  // readability.
  //
  // Returns the return value of `visitor`.
  template <class Visitor>
  auto VisitImpl(Visitor&& visitor) const {
    return std::visit(visitor, internal_->impl_);
  }

  // Returns total size of DataSlice, including missing items.
  size_t size() const { return GetShape().size(); }

  // Returns number of present items in DataSlice.
  size_t present_count() const {
    return VisitImpl([&]<class T>(const T& impl) -> size_t {
      if constexpr (std::is_same_v<T, internal::DataItem>) {
        return impl.has_value() ? 1 : 0;
      } else {
        return impl.present_count();
      }
    });
  }

  // In case of mixed types, returns NothingQType. While for DataSlice of
  // objects, returns ObjectIdQType.
  arolla::QTypePtr dtype() const {
    return VisitImpl([&](const auto& impl) { return impl.dtype(); });
  }

  // T can be internal::DataSliceImpl or internal::DataItem, depending on what
  // this DataSlice holds. It is a runtime error in case DataSlice does not hold
  // T.
  template <class T>
  const T& impl() const {
    return std::get<T>(internal_->impl_);
  }

  // Returns underlying implementation of DataSlice, if DataSliceImpl.
  const internal::DataSliceImpl& slice() const {
    return std::get<internal::DataSliceImpl>(internal_->impl_);
  }

  // Returns underlying implementation of DataSlice, if DataItem.
  const internal::DataItem& item() const {
    return std::get<internal::DataItem>(internal_->impl_);
  }

  // Returns true, if the underlying data is owned (DataItem holding a value or
  // DataSliceImpl holding DenseArrays). Allows converting the underlying value
  // to TypedRef in addition to TypedValue.
  bool impl_owns_value() const { return !impl_empty_and_unknown(); }

  // Returns true, if the slice does not contain any data and it does not know
  // the type of the underlying data (not related to Schema of the slice).
  bool impl_empty_and_unknown() const {
    return VisitImpl([&]<class T>(const T& impl) {
      if constexpr (std::is_same_v<T, internal::DataItem>) {
        return !impl.has_value();
      } else {
        return impl.is_empty_and_unknown();
      }
    });
  }

  // Returns true, if it holds values with different dtypes.
  bool impl_has_mixed_dtype() const {
    return VisitImpl([&]<class T>(const T& impl) {
      if constexpr (std::is_same_v<T, internal::DataItem>) {
        return false;
      } else {
        return impl.is_mixed_dtype();
      }
    });
  }

  // Returns a specialization key for creating a QValue subclass. DataSlice can
  // thus be used as an implementation for multiple QValue subclasses: DataItem,
  // ListItem, DictItem, Schema, etc.
  absl::string_view py_qvalue_specialization_key() const {
    return VisitImpl([&]<class T>(const T& impl) {
      if constexpr (std::is_same_v<T, internal::DataSliceImpl>) {
        return kDataSliceQValueSpecializationKey;
      } else {
        DCHECK_EQ(GetShape().rank(), 0);
        if (impl.is_list()) {
          return kListItemQValueSpecializationKey;
        } else if (impl.is_dict()) {
          return kDictItemQValueSpecializationKey;
        } else if (impl.is_schema() && GetSchemaImpl() == schema::kSchema) {
          return kSchemaItemQValueSpecializationKey;
        }
        return kDataItemQValueSpecializationKey;
      }
    });
  }

 private:
  using ImplVariant = std::variant<internal::DataItem, internal::DataSliceImpl>;

  DataSlice(ImplVariant impl, JaggedShapePtr shape, internal::DataItem schema,
            std::shared_ptr<DataBag> db = nullptr)
      : internal_(arolla::RefcountPtr<Internal>::Make(
            std::move(impl), std::move(shape), std::move(schema),
            std::move(db))) {}

  // Returns an Error if `schema` cannot be used for data whose type is defined
  // by `dtype`. `dtype` has a value of NothingQType in case the contents are
  // items with mixed types or no items are present (empty_and_unknown == true).
  static absl::Status VerifySchemaConsistency(const internal::DataItem& schema,
                                              arolla::QTypePtr dtype,
                                              bool empty_and_unknown);

  // Helper method for setting an attribute as if this DataSlice is a Schema
  // slice (schemas are stored in a dict and not in normal attribute storage).
  absl::Status SetSchemaAttr(absl::string_view attr_name,
                             const DataSlice& values) const;

  struct Internal : public arolla::RefcountedBase {
    ImplVariant impl_;
    // Can be shared between multiple DataSlice(s) (e.g. getattr, result
    // of all pointwise operators, as well as aggregation that returns the
    // same size - rank and similar).
    JaggedShapePtr shape_;
    // Schema:
    // * Primitive DType for primitive slices / items;
    // * ObjectId (allocated or UUID) for complex schemas, where it
    // represents a
    //   pointer to a start of schema definition in a DataBag.
    // * Special meaning DType. E.g. ANY, OBJECT, ITEM_ID, IMPLICIT,
    // EXPLICIT,
    //   etc. Please see go/kola-schema for details.
    internal::DataItem schema_;
    // Can be shared between multiple DataSlice(s) and underlying storage
    // can be changed outside of control of this DataSlice.
    std::shared_ptr<DataBag> db_;

    Internal() : shape_(JaggedShape::Empty()), schema_(schema::kAny) {}

    Internal(ImplVariant impl, JaggedShapePtr shape, internal::DataItem schema,
             std::shared_ptr<DataBag> db = nullptr)
        : impl_(std::move(impl)),
          shape_(std::move(shape)),
          schema_(std::move(schema)),
          db_(std::move(db)) {
      DCHECK(schema_.has_value());
    }
  };

  // RefcountPtr is used to ensure cheap DataSlice copying.
  arolla::RefcountPtr<Internal> internal_;
};

// Helper class for manipulating broadcasting of a DataSlice to a particular
// shape. Encapsulates the status error and the ownership of expanded value.
// Usage:
//   BroadcastHelper expanded(slice, shape);
//   RETURN_IF_ERROR(expanded.status());
//   expanded->...  // or (*expanded). ...
class BroadcastHelper {
 public:
  BroadcastHelper(const DataSlice& slice,
                  const DataSlice::JaggedShapePtr& shape)
      : ptr_(&slice) {
    if (slice.GetShape().IsEquivalentTo(*shape)) {
      return;
    }
    InitWithExpansionSlow(slice, shape);
  }

  BroadcastHelper(const BroadcastHelper&) = delete;
  BroadcastHelper(BroadcastHelper&&) = delete;

  const absl::Status& status() const { return status_; }
  const DataSlice& operator*() const { return *ptr_; }
  const DataSlice* operator->() const { return ptr_; }

 private:
  void InitWithExpansionSlow(const DataSlice& slice,
                             const DataSlice::JaggedShapePtr& shape);

  const DataSlice* ptr_;
  // Note: we use optional to avoid expensive DataSlice default constructor.
  std::optional<DataSlice> owned_expanded_;
  absl::Status status_ = absl::OkStatus();
};

// TODO: Remove this and use SetAttrs that sets (and casts)
// multiple attributes at the same time.
//
// Returns a new DataSlice which is a copy of the current value or casted
// according to the type of attribute `attr_name` of schema `lhs_schema`. If
// `update_schema=true` and `attr_name` schema attribute is missing from
// `lhs_schema`, it will be added. In case of conflicts or unsupported casting,
// the error is returned.
absl::StatusOr<DataSlice> CastOrUpdateSchema(
    const DataSlice& value, const internal::DataItem& lhs_schema,
    absl::string_view attr_name, bool update_schema,
    internal::DataBagImpl& db_impl);

}  // namespace koladata

#endif  // KOLADATA_DATA_SLICE_H_
