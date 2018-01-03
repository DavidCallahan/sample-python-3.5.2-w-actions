#pragma once
#include "Python.h"
#ifdef __cplusplus
extern "C" {
#endif
PyObject * do_binary_add(PyObject *left, PyObject*right,
                         PyCodeObject *code, uint32_t PC);
#ifdef __cplusplus
}
#endif

