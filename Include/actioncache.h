#pragma once
#include "Python.h"
#include <actionlist.h>
#include <unordered_map>

#include <iomanip>
#include <iostream>

#undef PROFILE_THRESHOLD
#define PROFILE_THRESHOLD 2

namespace Cache {
using word = intptr_t;
using ClassId = PyTypeObject *;
ClassId getClassId(PyObject *obj) { return Py_TYPE(obj); }

// place holder until there are caches
// in the code objects we combine hash code
// of the Code objects with the PC of the operation
// These are then mapped a structure which holds the
// the two classId's and the associated actions.

using CacheKey = std::pair<PyCodeObject *, uint32_t>;
struct CacheKeyHash {
  size_t operator()(const CacheKey &C) const {
    return uintptr_t(C.first) ^ C.second;
  }
};

template <unsigned Arity> struct CachedAction {
  unsigned profile = 1;
  std::array<ClassId, Arity> opIds;
  Action::ActionDataPtr action_;
  // initialize leftId_ to something invalid
  CachedAction() : action_(nullptr) { opIds[0] = nullptr; }
  bool match(std::array<PyObject *, Arity> args) {
    for (unsigned i = 0; i < Arity; i++) {
      if (opIds[i] != getClassId(args[i])) {
        return false;
      }
    }
    return true;
  }
  void setIds(std::array<PyObject *, Arity> args) {
    for (unsigned i = 0; i < Arity; i++) {
      opIds[i] = getClassId(args[i]);
    }
  }
};

template <size_t Arity>
class Cache {
public:
  using CacheT = std::unordered_map<CacheKey, CachedAction<Arity>, CacheKeyHash>;
  using ActionBuilder = Action::ActionDataPtr (*)(std::array<PyObject *, Arity>);
  Action::ActionData operator()(PyCodeObject *codeId, uint32_t pc,
                                ActionBuilder builder,
                                std::array<PyObject *, Arity> args);
  ~Cache();
private:
  CacheT cache;
  unsigned hits_ = 0;
  unsigned coldMisses_ = 0;
  unsigned misses_ = 0;
};

template <size_t Arity>
Action::ActionData Cache<Arity>::operator()(PyCodeObject *codeId, uint32_t pc,
                             ActionBuilder builder,
                             std::array<PyObject *, Arity> args) {
  auto key = std::make_pair(codeId, pc);
  auto Iter = cache.find(key);
  if (Iter != cache.end()) {
    auto &cachedAction = Iter->second;
    if (!cachedAction.match(args)) {
      misses_ += 1;
      cachedAction.setIds(args);
#ifdef PROFILE_THRESHOLD
      cachedAction.profile = -1;
      return nullptr;
#else
      cachedAction.action_ = std::move(builder(args));
#endif
    }
    else {
#ifdef PROFILE_THRESHOLD
      if (cachedAction.profile < 0) {
        return nullptr;
      }
      cachedAction.profile += 1;
      if (++cachedAction.profile < PROFILE_THRESHOLD) {
        return nullptr;
      }
#endif
      hits_ += 1;
    }
    return cachedAction.action_.get();
  }
  coldMisses_ += 1;
  auto &cachedAction = cache[key];
  cachedAction.setIds(args);
#ifdef PROFILE_THRESHOLD
  return nullptr;
#else
  cachedAction.action_ = std::move(builder(args));
  return cachedAction.action_.get();
#endif
}
  
template<size_t Arity>
Cache<Arity>::~Cache() {
  float hitRate = double(hits_)/double(coldMisses_ + misses_ + hits_);
  std::cerr << "for arity =" << Arity
     << " cold " << coldMisses_ << " hits " << hits_ << " misses "  << misses_
  << " rate " << hitRate << '\n';
}
} // namespace Cache
