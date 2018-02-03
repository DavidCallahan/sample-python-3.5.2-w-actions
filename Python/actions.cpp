
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

// Cache::Cache<2> binaryCache;
bool Action::DEBUG = false;
Cache::CacheStats Cache::stats;

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
  // TODO -- release captured references
  void captures(PyObject *obj) { Py_INCREF(obj); }

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
  void captures(PyObject *) {}

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

template <typename Evaluator>
PyObject *_PyObject_GenericGetAttrWithDict(Evaluator &eval, TypedObject obj,
                                           TypedObject name, PyObject *dict) {
  PyTypeObject *tp = Py_TYPE(obj);
  PyObject *descr = NULL;
  PyObject *res = NULL;
  descrgetfunc f;
  if (!PyUnicode_Check(name)) {
    return eval([](PyObject *obj, PyObject *name) -> PyObject * {
      PyErr_Format(PyExc_TypeError,
                   "attribute name must be string, not '%.200s'",
                   name->ob_type->tp_name);
      return NULL;
    });
  }
  Py_INCREF(name.getObject());
#ifndef NDEBUG
  const char *name_str = PyUnicode_AsUTF8(name.getObject());
  bool Debug = (strcmp(name_str, "_is_owned") == 0);
  PyObject *DebugObj = nullptr;
  if (Debug) {
    std::cerr << "get attr " << name_str << " from type " << tp->tp_name
              << '\n';
    DebugObj = PyObject_GetAttr(obj.getObject(), name.getObject());
  }
#endif

  if (tp->tp_dict == NULL) {
    if (PyType_Ready(tp) < 0)
      goto done;
  }

  descr = _PyType_Lookup(tp, name.getObject());
  Py_XINCREF(descr);

  f = NULL;
  if (descr != NULL) {
    f = descr->ob_type->tp_descr_get;
    if (f != NULL && PyDescr_IsData(descr)) {
      eval.captures(descr);
      res = eval([f, descr](PyObject *obj, PyObject *) {
        return f(descr, obj, (PyObject *)obj->ob_type);
      });
      goto done;
    }
  }

  assert(dict == NULL && "Unexpected dict in eval version of GetAttr");
  if (dict == NULL) {
    Py_ssize_t dictoffset = tp->tp_dictoffset;
    if (dictoffset != 0) {
      res = eval([tp](PyObject *obj, PyObject *name) {
        PyObject *dict = NULL;
        Py_ssize_t dictoffset = tp->tp_dictoffset;
        /* Inline _PyObject_GetDictPtr */
        if (dictoffset < 0) {
          Py_ssize_t tsize;
          size_t size;

          tsize = ((PyVarObject *)obj)->ob_size;
          if (tsize < 0)
            tsize = -tsize;
          size = _PyObject_VAR_SIZE(tp, tsize);

          dictoffset += (long)size;
          assert(dictoffset > 0);
          assert(dictoffset % SIZEOF_VOID_P == 0);
        }
        PyObject **dictptr = (PyObject **)((char *)obj + dictoffset);
        dict = *dictptr;
        if (dict != NULL) {
          Py_INCREF(dict);
          PyObject *res = PyDict_GetItem(dict, name);
          if (res != NULL) {
            Py_INCREF(res);
            Py_DECREF(dict);
            return res;
          }
          Py_DECREF(dict);
        }
        return Py_NotImplemented;
      });
      if (res != Py_NotImplemented)
        goto done;
      res = NULL;
    }
  }

  if (f != NULL) {
    eval.captures(descr);
    res = eval([f, descr](PyObject *obj, PyObject *) {
      return f(descr, obj, (PyObject *)Py_TYPE(obj));
    });
    goto done;
  }

  if (descr != NULL) {
    eval.captures(descr);
    res = eval([descr](PyObject *, PyObject *name) {
      Py_INCREF(descr);
      return descr;
    });
    // todo Py_DECREF(descr) -- taken by capture
    descr = NULL;
    goto done;
  }

  eval([tp](PyObject *, PyObject *name) -> PyObject * {
    PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%U'",
                 tp->tp_name, name);
    return NULL;
  });
done:
  Py_XDECREF(descr);
  Py_DECREF(name.getObject());
  return res;
}

template <typename Evaluator>
PyObject *PyObject_GenericGetAttr(Evaluator &eval, TypedObject obj,
                                  TypedObject name) {
  return _PyObject_GenericGetAttrWithDict(eval, obj, name, nullptr);
}

template <typename Evaluator>
PyObject *PyObject_GetAttr(Evaluator &eval, TypedObject v, TypedObject name) {
  PyTypeObject *tp = Py_TYPE(v);

  if (!PyUnicode_Check(name)) {
    return eval([](PyObject *, PyObject *name) -> PyObject * {
      PyErr_Format(PyExc_TypeError,
                   "attribute name must be string, not '%.200s'",
                   name->ob_type->tp_name);
      return NULL;
    });
  }
  if (auto *tp_getattro = tp->tp_getattro) {
    if (tp_getattro != ::PyObject_GenericGetAttr) {
      return eval([tp_getattro](PyObject *v, PyObject *name) {
        return tp_getattro(v, name);
      });
    }
    return PyObject_GenericGetAttr(eval, v, name);
  }
  if (auto *tp_getattr = tp->tp_getattr) {
    return eval([tp_getattr](PyObject *v, PyObject *name) -> PyObject * {
      char *name_str = _PyUnicode_AsString(name);
      if (name_str == NULL)
        return NULL;
      return tp_getattr(v, name_str);
    });
  }
  return eval([tp](PyObject *, PyObject *name) -> PyObject * {
    PyErr_Format(PyExc_AttributeError, "'%.50s' object has no attribute '%U'",
                 tp->tp_name, name);
    return NULL;
  });
}

ActionDataPtr binary_add_action(ActionList<2>::ArgList args) {
  RecordActions<2> recorder(args);
  PyNumber_Add(recorder, args[0], args[1]);
  return recorder.data();
}
ActionDataPtr load_attr_action(ActionList<2>::ArgList args) {
  RecordActions<2> recorder(args);
  PyObject_GetAttr(recorder, args[0], args[1]);
  return recorder.data();
}
#if 0
  PyObject *generic_operation(PyCodeObject *code, uint32_t PC,
                              typename Cache::Cache<2>::ActionBuilder builder,
                              typename ActionList<2>::ArgList args) {
    ActionData action =  binaryCache(code, PC, builder, args);
    return ActionList<2>::run(action, args);
  }
#endif
PyObject *generic_operation(typename ActionList<2>::ArgList args,
                            Cache::CachedAction<2> *cache,
                            PyObject *(*defaultAction)(PyObject *, PyObject *),
                            typename Cache::Cache<2>::ActionBuilder builder) {

  if (cache->profile == PROFILE_THRESHOLD) {
    if (cache->match(args)) {
      return ActionList<2>::run(cache->action_.get(), args);
    }
    cache->profile = -1;
  } else if (cache->profile++ == 0) {
    cache->setIds(args);
  } else if (!cache->match(args)) {
    cache->profile = -1;
  } else if (cache->profile == PROFILE_THRESHOLD) {
    cache->action_ = builder(args);
    return ActionList<2>::run(cache->action_.get(), args);
  }
  return defaultAction(args[0], args[1]);
}
} // namespace

extern "C" {
PyObject *PyNumber_Add(PyObject *v, PyObject *w) {
  EvalAction<2> eval({{v, w}});
  return PyNumber_Add(eval, v, w);
}

PyObject *do_binary_add(PyObject *left, PyObject *right, void **cache_) {
  auto *cache = reinterpret_cast<Cache::CachedAction<2> *>(cache_);
  return generic_operation({{left, right}}, cache, ::PyNumber_Add,
                           binary_add_action);
}
PyObject *do_load_attr(PyObject *obj, PyObject *name, void **cache_) {
#if PROFILE_THRESHOLD
  auto *cache = reinterpret_cast<Cache::CachedAction<2> *>(cache_);
  return generic_operation({{obj, name}}, cache, ::PyObject_GetAttr,
                           load_attr_action);
#else
  EvalAction<2> eval({{obj, name}});
  return PyObject_GetAttr(eval, obj, name);
#endif
}
}
