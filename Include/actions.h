#pragma once
#include "Python.h"
#ifdef __cplusplus
extern "C" {
#endif
PyObject *do_binary_add(PyObject *left, PyObject *right, void **cache);
PyObject *do_load_attr(PyObject *left, PyObject *right, void** cache);
#ifdef __cplusplus
}
#endif
