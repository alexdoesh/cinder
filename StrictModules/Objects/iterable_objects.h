// Copyright (c) Facebook, Inc. and its affiliates. (http://www.facebook.com)
#ifndef __STRICTM_ITERABLE_OBJ___
#define __STRICTM_ITERABLE_OBJ___
#include "StrictModules/Objects/instance.h"
#include "StrictModules/Objects/object_type.h"

#include <unordered_set>

namespace strictmod::objects {
// Iterable (models non random access python iterable)
class StrictIterable : public StrictInstance {
 public:
  using StrictInstance::StrictInstance;
};

class StrictIterableType : public StrictObjectType {
 public:
  using StrictObjectType::StrictObjectType;
  // TODO: getElements
};

// Sequence (random access)
class StrictSequence : public StrictIterable {
 public:
  StrictSequence(
      std::shared_ptr<StrictType> type,
      std::weak_ptr<StrictModuleObject> creator,
      std::vector<std::shared_ptr<BaseStrictObject>> data);

  StrictSequence(
      std::shared_ptr<StrictType> type,
      std::shared_ptr<StrictModuleObject> creator,
      std::vector<std::shared_ptr<BaseStrictObject>> data);

  const std::vector<std::shared_ptr<BaseStrictObject>>& getData() {
    return data_;
  }

  void setData(size_t i, std::shared_ptr<BaseStrictObject> v) {
    data_[i] = std::move(v);
  }
  // Factory method
  virtual std::shared_ptr<StrictSequence> makeSequence(
      std::shared_ptr<StrictType> type,
      std::weak_ptr<StrictModuleObject> creator,
      std::vector<std::shared_ptr<BaseStrictObject>> data) = 0;

  // wrapped methods
  static std::shared_ptr<BaseStrictObject> sequence__contains__(
      std::shared_ptr<StrictSequence> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> element);

  static std::shared_ptr<BaseStrictObject> sequence__len__(
      std::shared_ptr<StrictSequence> self,
      const CallerContext& caller);

  static std::shared_ptr<BaseStrictObject> sequence__iter__(
      std::shared_ptr<StrictSequence> self,
      const CallerContext& caller);

  static std::shared_ptr<BaseStrictObject> sequence__eq__(
      std::shared_ptr<StrictSequence> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> rhs);

  static std::shared_ptr<BaseStrictObject> sequence__add__(
      std::shared_ptr<StrictSequence> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> rhs);

  static std::shared_ptr<BaseStrictObject> sequence__mul__(
      std::shared_ptr<StrictSequence> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> rhs);

  static std::shared_ptr<BaseStrictObject> sequence__rmul__(
      std::shared_ptr<StrictSequence> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> lhs);

  // TODO: __reversed__
 protected:
  std::vector<std::shared_ptr<BaseStrictObject>> data_;
};

class StrictSequenceType : public StrictIterableType {
 public:
  using StrictIterableType::StrictIterableType;

  virtual std::shared_ptr<BaseStrictObject> getElement(
      std::shared_ptr<BaseStrictObject> obj,
      std::shared_ptr<BaseStrictObject> index,
      const CallerContext& caller) override;

  virtual std::shared_ptr<StrictIteratorBase> getElementsIter(
      std::shared_ptr<BaseStrictObject> obj,
      const CallerContext& caller) override;

  virtual std::vector<std::shared_ptr<BaseStrictObject>> getElementsVec(
      std::shared_ptr<BaseStrictObject> obj,
      const CallerContext& caller) override;

  virtual void addMethods() override;
};

// List
class StrictList final : public StrictSequence {
 public:
  using StrictSequence::StrictSequence;

  virtual std::shared_ptr<StrictSequence> makeSequence(
      std::shared_ptr<StrictType> type,
      std::weak_ptr<StrictModuleObject> creator,
      std::vector<std::shared_ptr<BaseStrictObject>> data) override;

  virtual std::string getDisplayName() const override;
  virtual Ref<> getPyObject() const override;

  // wrapped methods
  static std::shared_ptr<BaseStrictObject> listAppend(
      std::shared_ptr<StrictList> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> elem);

  static std::shared_ptr<BaseStrictObject> listCopy(
      std::shared_ptr<StrictList> self,
      const CallerContext& caller);

  static std::shared_ptr<BaseStrictObject> list__init__(
      std::shared_ptr<StrictList> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> iterable = nullptr);

  static std::shared_ptr<BaseStrictObject> listExtend(
      std::shared_ptr<StrictList> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> iterable);
};

class StrictListType final : public StrictSequenceType {
 public:
  using StrictSequenceType::StrictSequenceType;

  virtual void setElement(
      std::shared_ptr<BaseStrictObject> obj,
      std::shared_ptr<BaseStrictObject> index,
      std::shared_ptr<BaseStrictObject> value,
      const CallerContext& caller) override;

  virtual std::unique_ptr<BaseStrictObject> constructInstance(
      std::shared_ptr<StrictModuleObject> caller) override;

  virtual Ref<> getPyObject() const override;

  virtual void addMethods() override;
};

// Tuple
class StrictTuple final : public StrictSequence {
 public:
  StrictTuple(
      std::shared_ptr<StrictType> type,
      std::weak_ptr<StrictModuleObject> creator,
      std::vector<std::shared_ptr<BaseStrictObject>> data);

  StrictTuple(
      std::shared_ptr<StrictType> type,
      std::shared_ptr<StrictModuleObject> creator,
      std::vector<std::shared_ptr<BaseStrictObject>> data);

  virtual std::shared_ptr<StrictSequence> makeSequence(
      std::shared_ptr<StrictType> type,
      std::weak_ptr<StrictModuleObject> creator,
      std::vector<std::shared_ptr<BaseStrictObject>> data) override;

  virtual bool isHashable() const override;
  virtual size_t hash() const override;
  virtual bool eq(const BaseStrictObject& other) const override;

  virtual std::string getDisplayName() const override;
  virtual Ref<> getPyObject() const override;

  // wrapped methods
  static std::shared_ptr<BaseStrictObject> tupleIndex(
      std::shared_ptr<StrictTuple> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> item);

  static std::shared_ptr<BaseStrictObject> tuple__new__(
      std::shared_ptr<StrictTuple> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> instType,
      std::shared_ptr<BaseStrictObject> elements = nullptr);

 private:
  mutable Ref<> pyObj_;
  mutable std::string displayName_;
};

class StrictTupleType final : public StrictSequenceType {
 public:
  using StrictSequenceType::StrictSequenceType;

  virtual std::unique_ptr<BaseStrictObject> constructInstance(
      std::shared_ptr<StrictModuleObject> caller) override;

  virtual Ref<> getPyObject() const override;

  virtual void addMethods() override;
};

typedef std::unordered_set<
    std::shared_ptr<BaseStrictObject>,
    StrictObjectHasher,
    StrictObjectEqual>
    SetDataT;

/**
 * SetLike, base class for Set and FrozenSet
 * Technically, elements used in a set has to be hashable in Python semantics
 * and the hash function/ equality function should be looked up using
 * __hash__ and __eq__
 * For simplicity and to reduce overhead, we allow any kind of object in the
 * analysis, and use naive object identity except for builtin types
 */
class StrictSetLike : public StrictIterable {
 public:
  StrictSetLike(
      std::shared_ptr<StrictType> type,
      std::weak_ptr<StrictModuleObject> creator,
      SetDataT data = SetDataT());

  StrictSetLike(
      std::shared_ptr<StrictType> type,
      std::shared_ptr<StrictModuleObject> creator,
      SetDataT data = {});

  // Factory method
  virtual std::shared_ptr<StrictSetLike> makeSetLike(
      std::shared_ptr<StrictType> type,
      std::weak_ptr<StrictModuleObject> creator,
      SetDataT data) = 0;

  const SetDataT& getData() {
    return data_;
  }

  void addElement(
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> element);

  // wrapped methods
  static std::shared_ptr<BaseStrictObject> set__contains__(
      std::shared_ptr<StrictSetLike> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> element);

  static std::shared_ptr<BaseStrictObject> set__len__(
      std::shared_ptr<StrictSetLike> self,
      const CallerContext& caller);

  static std::shared_ptr<BaseStrictObject> set__and__(
      std::shared_ptr<StrictSetLike> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> rhs);

  static std::shared_ptr<BaseStrictObject> set__or__(
      std::shared_ptr<StrictSetLike> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> rhs);

  static std::shared_ptr<BaseStrictObject> set__xor__(
      std::shared_ptr<StrictSetLike> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> rhs);

  static std::shared_ptr<BaseStrictObject> set__iter__(
      std::shared_ptr<StrictSetLike> self,
      const CallerContext& caller);

  // TODO issubset, issuperset, __le__, __lt__,
  // __ge__, __gt__
 protected:
  SetDataT data_;
};

class StrictSetLikeType : public StrictObjectType {
 public:
  using StrictObjectType::StrictObjectType;

  virtual void addMethods() override;

  virtual std::shared_ptr<StrictIteratorBase> getElementsIter(
      std::shared_ptr<BaseStrictObject> obj,
      const CallerContext& caller) override;

  virtual std::vector<std::shared_ptr<BaseStrictObject>> getElementsVec(
      std::shared_ptr<BaseStrictObject> obj,
      const CallerContext& caller) override;
};

// Set
class StrictSet final : public StrictSetLike {
 public:
  using StrictSetLike::StrictSetLike;

  virtual std::string getDisplayName() const override;
  virtual Ref<> getPyObject() const override;

  virtual std::shared_ptr<StrictSetLike> makeSetLike(
      std::shared_ptr<StrictType> type,
      std::weak_ptr<StrictModuleObject> creator,
      SetDataT data) override;

  // wrapped methods

  static std::shared_ptr<BaseStrictObject> setAdd(
      std::shared_ptr<StrictSet> self,
      const CallerContext& caller,
      std::shared_ptr<BaseStrictObject> item);

  // TODO  __init__, update,
};

class StrictSetType final : public StrictSetLikeType {
 public:
  using StrictSetLikeType::StrictSetLikeType;
  virtual std::unique_ptr<BaseStrictObject> constructInstance(
      std::shared_ptr<StrictModuleObject> caller) override;

  virtual void addMethods() override;
};

// FrozenSet
class StrictFrozenSet final : public StrictSetLike {
 public:
  StrictFrozenSet(
      std::shared_ptr<StrictType> type,
      std::weak_ptr<StrictModuleObject> creator,
      SetDataT data = {});

  StrictFrozenSet(
      std::shared_ptr<StrictType> type,
      std::shared_ptr<StrictModuleObject> creator,
      SetDataT data = {});

  virtual std::string getDisplayName() const override;
  virtual Ref<> getPyObject() const override;

  virtual std::shared_ptr<StrictSetLike> makeSetLike(
      std::shared_ptr<StrictType> type,
      std::weak_ptr<StrictModuleObject> creator,
      SetDataT data) override;

  // wrapped methods
  // TODO __new__,

 private:
  mutable Ref<> pyObj_;
  mutable std::string displayName_;
};

class StrictFrozenSetType final : public StrictSetLikeType {
 public:
  using StrictSetLikeType::StrictSetLikeType;
  virtual std::unique_ptr<BaseStrictObject> constructInstance(
      std::shared_ptr<StrictModuleObject> caller) override;

  virtual void addMethods() override;
};
} // namespace strictmod::objects
#endif //__STRICTM_ITERABLE_OBJ___
