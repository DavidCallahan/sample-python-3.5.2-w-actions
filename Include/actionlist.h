#pragma once
#include <vector>
#include <array>
#include <memory>
#include <cassert>

#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
TypeName(const TypeName&) = delete;      \
void operator=(const TypeName&) = delete

#define DISALLOW_HEAP_ALLOCATION()          \
void* operator new(size_t size) = delete; \
void operator delete(void* p) = delete

#define DISALLOW_IMPLICIT_CONSTRUCTORS(TypeName) \
TypeName() = delete;                           \
DISALLOW_COPY_AND_ASSIGN(TypeName)


namespace Action {
  extern bool DEBUG;
  
  inline PyObject * null_object() {
    return nullptr;
  }
  // This class represents the "action" which is determined by
  // PyObject types but which acts on PyObject values. We will cache
  // this action expecting it to be reused and not recomputed.
  
  // This abstraction is a placeholder for eventually JITing a dedicated
  // function which will have action member values embedded as constants
  
  using ActionData = intptr_t *;
  using ActionDataPtr = std::unique_ptr<intptr_t>;

  
  template<unsigned Arity>
  class ActionList {
  public:
    explicit ActionList() {
      Data_.push_back(0);
    }
    using ArgList = std::array<PyObject*,Arity>;
    template<typename Function>
    PyObject* operator()(Function F);
    
    PyObject* operator()(unaryfunc);
    PyObject* operator()(binaryfunc);
    PyObject* operator()(ternaryfunc);
    
    static PyObject * run(intptr_t * data, ArgList args);
    void run(ArgList args) { run(data(), args); }
    std::unique_ptr<intptr_t> data() {
      Data_.front() = Data_.size();
      intptr_t * data = new intptr_t[Data_.size()];
      memcpy(data, Data_.data(), Data_.size()*sizeof(intptr_t));
      std::unique_ptr<intptr_t> result(data);
      return result;
    }
  private:
    static PyObject * simple_call(intptr_t i, ArgList);
    
    struct MockLambda {
      PyObject * operator()(ActionList::ArgList) { return nullptr; }
    };
    std::vector<intptr_t> Data_;
    DISALLOW_COPY_AND_ASSIGN(ActionList);
  };
  
  size_t ceil(size_t dividend, size_t divisor) {
    return (dividend + divisor -1)/divisor;
  }
  
  struct PtrToMember {
    void *ptr;
    size_t offset;
  };
  
  template<unsigned Arity>
  template<typename Function>
  PyObject* ActionList<Arity>::operator()(Function F) {
    auto f = &Function::operator();
    assert(sizeof(f) == 2*sizeof(intptr_t));
    auto* p = reinterpret_cast<intptr_t*>(&f);
    assert(p[1] == 0);
    auto n = (sizeof(Function) == 1  ? 0 :
              ceil(sizeof(Function), sizeof(intptr_t)));
    if (n < sizeof(intptr_t)-2) {
      Data_.push_back(*p + n+1);
    }
    else {
      Data_.push_back(*p + sizeof(intptr_t)-1);
      Data_.push_back(n);
    }
    auto * FP = reinterpret_cast<intptr_t*>(&F);
    Data_.insert(Data_.end(), FP, FP+n);
    return Py_NotImplemented;
  }
  
  template<>
  PyObject* ActionList<2>::operator()(binaryfunc F) {
    Data_.push_back(reinterpret_cast<intptr_t>(F));
    return Py_NotImplemented;
  }
  template<>
  PyObject* ActionList<3>::operator()(ternaryfunc F) {
    Data_.push_back(reinterpret_cast<intptr_t>(F));
    return Py_NotImplemented;
  }
  
  template<>
  PyObject* ActionList<2>::simple_call(intptr_t i, ArgList args) {
    auto *f = reinterpret_cast<PyObject*(*)(PyObject*,PyObject*)>(i);
    return f(args[0], args[1]);
  }
  
  
  template<unsigned Arity>
  PyObject * ActionList<Arity>::run(intptr_t *data, ArgList args) {
    auto size = *data;
    auto end = data+size;;
    auto cur = data+1;
    auto f = &MockLambda::operator();
    auto *fAsInts = reinterpret_cast<intptr_t*>(&f);
    const uintptr_t MASK = (sizeof(intptr_t)-1);
    PyObject * res;
    while (cur != end) {
      intptr_t i = *cur++;
      fAsInts[0] = (i &~ MASK);
      intptr_t n = i & MASK;
      if (n == 0) {
        res = simple_call(i,args);
        if (res != Py_NotImplemented) {
          return res;
        }
        continue;
      }
      if (n == MASK) {
        n = *cur++;
      }
      else {
        n -= 1; // encoded size is biased by 1.
      }
      // lambda operator of n args;
      MockLambda *e = reinterpret_cast<MockLambda*>(cur);
      res = (e->*f)(args);
      if (res != Py_NotImplemented) {
        return res;
      }
      cur += n;
    }
    assert(!"no action found");
    return null_object();
  }
}
