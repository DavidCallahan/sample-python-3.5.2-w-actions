#pragma once
#include "Python.h"
#include <actionlist.h>
#include <unordered_map>

#include <iomanip>
#include <iostream>

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
using CacheT = std::unordered_map<CacheKey, CachedAction<Arity>, CacheKeyHash>;

// extern CacheT<1> unaryCache;
extern CacheT<2> binaryCache;
// extern CacheT<3> ternaryCache;

template <unsigned Arity> CacheT<Arity> &getCache();
// template<> CacheT<1> & getCache() { return unaryCache; }
template <> CacheT<2> &getCache() { return binaryCache; }
// template<> CacheT<3> & getCache() { return ternaryCache; }

template <size_t Arity>
using ActionBuilder = Action::ActionDataPtr (*)(std::array<PyObject *, Arity>);

template <size_t Arity>
Action::ActionData operation(PyCodeObject *codeId, uint32_t pc,
                             ActionBuilder<Arity> builder,
                             std::array<PyObject *, Arity> args) {
  auto key = std::make_pair(codeId, pc);
  auto &cache = getCache<Arity>();
  auto Iter = cache.find(key);
  if (Iter != cache.end()) {
    auto &cachedAction = Iter->second;
    if (!cachedAction.match(args)) {
      if (Action::DEBUG)
        std::cerr << "miss " << std::hex << codeId << " " << std::dec << pc
                  << '\n';
      cachedAction.setIds(args);
      cachedAction.action_ = std::move(builder(args));
    }
    if (Action::DEBUG)
      std::cerr << "hit " << std::hex << codeId << " " << std::dec << pc
                << '\n';
    return cachedAction.action_.get();
  }
  if (Action::DEBUG)
    std::cerr << "new " << std::hex << codeId << " " << std::dec << pc << '\n';
  auto &cachedAction = cache[key];
  cachedAction.setIds(args);
  cachedAction.action_ = std::move(builder(args));
  return cachedAction.action_.get();
}
} // namespace Cache
