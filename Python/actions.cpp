
#include <Python.h>
#include <object.h>

#include <actioncache.h>
#include <actionlist.h>
#include <actions.h>
#include <iostream>

#include <array>
#include <memory>
#include <vector>

// from abstract.c
#define NB_SLOT(x) offsetof(PyNumberMethods, x)
#define NB_BINOP(nb_methods, slot)                                             \
  (*(binaryfunc *)(&((char *)nb_methods)[slot]))
#define NB_TERNOP(nb_methods, slot)                                            \
  (*(ternaryfunc *)(&((char *)nb_methods)[slot]))

#undef Py_TYPE
PyTypeObject *Py_TYPE(PyObject *o) { return o->ob_type; }

Cache::CacheT<2> Cache::binaryCache;
bool Action::DEBUG = false;

namespace {
using namespace Action;

template <size_t N> class RecordActions : public ActionList<N> {
public:
  using ArgList = typename ActionList<N>::ArgList;
  explicit RecordActions(ArgList args) : args_(args) {}
  template <typename Function> PyObject *operator()(Function f) {
    return (*((ActionList<2> *)this))(f);
  }
  PyObject *operator()(binaryfunc);

private:
  ArgList args_;
  DISALLOW_IMPLICIT_CONSTRUCTORS(RecordActions);
};

using binaryfunc_action = PyObject *(*)(RecordActions<2> &, PyObject *,
                                        PyObject *);
std::unordered_map<binaryfunc, binaryfunc_action> binaryFunctionMap;
template <> PyObject *RecordActions<2>::operator()(binaryfunc f) {
  auto Iter = binaryFunctionMap.find(f);
  if (Iter == binaryFunctionMap.end()) {
    return (*((ActionList<2> *)this))(f);
  }
  return Iter->second(*this, args_[0], args_[1]);
}

template <unsigned N> class EvalAction {
public:
  using ArgList = std::array<PyObject *, N>;
  explicit EvalAction(ArgList args) : args_(args) {}
  PyObject *operator()(unaryfunc f);
  PyObject *operator()(binaryfunc f);
  PyObject *operator()(ternaryfunc f);
  template <typename Function> PyObject *operator()(Function f);

  template <typename Function> PyObject *call(Function f) { return f(args_); }

private:
  ArgList args_;
};

template <> PyObject *EvalAction<1>::operator()(unaryfunc f) {
  return f(args_[0]);
}

template <> PyObject *EvalAction<2>::operator()(binaryfunc f) {
  return f(args_[0], args_[1]);
}
template <> PyObject *EvalAction<3>::operator()(ternaryfunc f) {
  return f(args_[0], args_[1], args_[2]);
}

template <>
template <typename Function>
PyObject *EvalAction<1>::operator()(Function f) {
  return f(args_[0]);
}
template <>
template <typename Function>
PyObject *EvalAction<2>::operator()(Function f) {
  return f(args_[0], args_[1]);
}
template <>
template <typename Function>
PyObject *EvalAction<3>::operator()(Function f) {
  return f(args_[0], args_[1], args_[2]);
}

// Cache the type of an PyObject o accelerate access for immediate values.
// This wrapper is used for paramater types to transformed functions to hide
// 1. Accerlate access to the type of an PyObject by caching a copy
// 2. Make it explicit where the PyObject value and not its type is used.
class TypedObject {
public:
  TypedObject(PyObject *obj) : obj_(obj), type_(Py_TYPE(obj)) {}
  TypedObject(const TypedObject &other)
      : obj_(other.obj_), type_(other.type_) {}
  PyObject *getObject() const { return obj_; }
  PyTypeObject *getType() const { return type_; }
  operator PyTypeObject *() { return type_; }

private:
  PyObject *obj_;
  PyTypeObject *type_;
};
inline PyTypeObject *Py_TYPE(TypedObject t) { return t.getType(); }

template <typename Evaluator>
PyObject *binary_op1(Evaluator &eval, TypedObject v, TypedObject w,
                     const int op_slot) {
  PyObject *x;
  binaryfunc slotv = NULL;
  binaryfunc slotw = NULL;
  if (Action::DEBUG)
    std::cerr << "binary op1 " << v.getType()->tp_name << '\n';

  if (Py_TYPE(v)->tp_as_number != NULL)
    slotv = NB_BINOP(Py_TYPE(v)->tp_as_number, op_slot);
  if (Py_TYPE(w) != Py_TYPE(v) && Py_TYPE(w)->tp_as_number != NULL) {
    slotw = NB_BINOP(Py_TYPE(w)->tp_as_number, op_slot);
    if (slotw == slotv)
      slotw = NULL;
  }
  if (slotv) {
    if (slotw && PyType_IsSubtype(Py_TYPE(w), Py_TYPE(v))) {
      x = eval(slotv);
      if (x != Py_NotImplemented)
        return x;
      slotw = NULL;
    }
    x = eval(slotv);
    if (x != Py_NotImplemented)
      return x;
  }
  if (slotw) {
    x = eval(slotw);
    if (x != Py_NotImplemented)
      return x;
  }
  Py_RETURN_NOTIMPLEMENTED;
}

template <typename Evaluator>
PyObject *binop_type_error(Evaluator &eval, TypedObject, TypedObject,
                           const char *op_name) {
  return eval([op_name](PyObject *v, PyObject *w) {
    PyErr_Format(PyExc_TypeError,
                 "unsupported operand type(s) for %.100s: "
                 "'%.100s' and '%.100s'",
                 op_name, v->ob_type->tp_name, w->ob_type->tp_name);
    return null_object();
  });
}

template <typename Evaluator>
PyObject *PyNumber_Add(Evaluator &eval, TypedObject v, TypedObject w) {
  PyObject *result = binary_op1(eval, v, w, NB_SLOT(nb_add));
  if (result == Py_NotImplemented) {
    PySequenceMethods *m = Py_TYPE(v)->tp_as_sequence;
    if (m && m->sq_concat) {
      return eval([m](PyObject *v, PyObject *w) {
        if (Action::DEBUG)
          std::cerr << "sq concat " << v->ob_type->tp_name;
        return (*m->sq_concat)(v, w);
      });
    }
    result = binop_type_error(eval, v, w, "+");
  }
  return result;
}

ActionDataPtr binary_add_action(ActionList<2>::ArgList args) {
  RecordActions<2> recorder(args);
  PyNumber_Add(recorder, args[0], args[1]);
  return recorder.data();
}
} // namespace

extern "C" {
PyObject *PyNumber_Add_E(PyObject *v, PyObject *w) {
  EvalAction<2> eval({{v, w}});
  return PyNumber_Add(eval, v, w);
}

PyObject *do_binary_add(PyObject *left, PyObject *right,
                        PyCodeObject *co, uint32_t PC) {
#if 0
  return PyNumber_Add_E(left, right);
#else
  ActionData action = Cache::operation<2>(co, PC,
                                        binary_add_action,
                                        {{left,right}});
  return ActionList<2>::run(action, {{left, right}});
#endif
}
}
