// Copyright (c) Facebook, Inc. and its affiliates. (http://www.facebook.com)
#ifndef __STRICTMOD_CONSTANTS_H__
#define __STRICTMOD_CONSTANTS_H__

#include "StrictModules/Objects/instance.h"

namespace strictmod::objects {
class NoneObject_ : public StrictInstance {
  using StrictInstance::StrictInstance;

  virtual Ref<> getPyObject() const override;
  virtual std::string getDisplayName() const override;
};

class NotImplementedObject : public StrictInstance {
  using StrictInstance::StrictInstance;

  virtual Ref<> getPyObject() const override;
  virtual std::string getDisplayName() const override;
};

} // namespace strictmod::objects

#endif // __STRICTMOD_CONSTANTS_H__
