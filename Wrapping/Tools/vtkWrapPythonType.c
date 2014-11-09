/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkWrapPythonType.c

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkWrapPythonType.h"
#include "vtkWrapPythonClass.h"
#include "vtkWrapPythonMethod.h"
#include "vtkWrapPythonMethodDef.h"
#include "vtkWrapPythonTemplate.h"

#include "vtkWrap.h"
#include "vtkParseExtras.h"

/* required for VTK_BUILD_SHARED_LIBS */
#include "vtkConfigure.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* -------------------------------------------------------------------- */
/* A struct for special types to store info about the type, it is fairly
 * small because not many operators or special features are wrapped */
typedef struct _SpecialTypeInfo
{
  int has_print;    /* there is "<<" stream operator */
  int has_compare;  /* there are comparison operators e.g. "<" */
  int has_sequence; /* the [] operator takes a single integer */
} SpecialTypeInfo;

/* -------------------------------------------------------------------- */
/* The following functions are for generating code for special types,
 * i.e. types that are not derived from vtkObjectBase */
/* -------------------------------------------------------------------- */

/* -------------------------------------------------------------------- */
/* generate function for printing a special object */
static void vtkWrapPython_NewDeleteProtocol(
  FILE *fp, const char *classname, ClassInfo *data)
{
  const char *constructor;
  size_t n, m;

  /* remove namespaces and template parameters from the
   * class name to get the constructor name */
  constructor = data->Name;
  m = vtkParse_UnscopedNameLength(constructor);
  while (constructor[m] == ':' && constructor[m+1] == ':')
    {
    constructor += m + 2;
    m = vtkParse_UnscopedNameLength(constructor);
    }
  for (n = 0; n < m; n++)
    {
    if (constructor[n] == '<')
      {
      break;
      }
    }

  /* the new method for python versions >= 2.2 */
  fprintf(fp,
    "#if PY_VERSION_HEX >= 0x02020000\n"
    "static PyObject *\n"
    "Py%s_New(PyTypeObject *, PyObject *args, PyObject *kwds)\n"
    "{\n"
    "  if (kwds && PyDict_Size(kwds))\n"
    "    {\n"
    "    PyErr_SetString(PyExc_TypeError,\n"
    "                    \"this function takes no keyword arguments\");\n"
    "    return NULL;\n"
    "    }\n"
    "\n"
    "  return Py%s_%*.*s(NULL, args);\n"
    "}\n"
    "#endif\n"
    "\n",
    classname, classname, (int)n, (int)n, constructor);

  /* the delete method */
  fprintf(fp,
    "static void Py%s_Delete(PyObject *self)\n"
    "{\n"
    "  PyVTKSpecialObject *obj = (PyVTKSpecialObject *)self;\n"
    "  if (obj->vtk_ptr)\n"
    "    {\n"
    "    delete static_cast<%s *>(obj->vtk_ptr);\n"
    "    }\n"
    "#if PY_MAJOR_VERSION >= 2\n"
    "  PyObject_Del(self);\n"
    "#else\n"
    "  PyMem_DEL(self);\n"
    "#endif\n"
    "}\n"
    "\n",
    classname, data->Name);
}


/* -------------------------------------------------------------------- */
/* generate function for printing a special object */
static void vtkWrapPython_PrintProtocol(
  FILE *fp, const char *classname, ClassInfo *data,
  FileInfo *finfo, SpecialTypeInfo *info)
{
  int i;
  FunctionInfo *func;

  /* look in the file for "operator<<" for printing */
  for (i = 0; i < finfo->Contents->NumberOfFunctions; i++)
    {
    func = finfo->Contents->Functions[i];
    if (func->Name && func->IsOperator &&
        strcmp(func->Name, "operator<<") == 0)
      {
      if (func->NumberOfParameters == 2 &&
          (func->Parameters[0]->Type & VTK_PARSE_UNQUALIFIED_TYPE) ==
              VTK_PARSE_OSTREAM_REF &&
          (func->Parameters[1]->Type & VTK_PARSE_BASE_TYPE) ==
              VTK_PARSE_OBJECT &&
          (func->Parameters[1]->Type & VTK_PARSE_POINTER_MASK) == 0 &&
          strcmp(func->Parameters[1]->Class, data->Name) == 0)
        {
        info->has_print = 1;
        }
      }
    }

  /* the str function */
  if (info->has_print)
    {
    fprintf(fp,
      "static PyObject *Py%s_String(PyObject *self)\n"
      "{\n"
      "  PyVTKSpecialObject *obj = (PyVTKSpecialObject *)self;\n"
      "  vtksys_ios::ostringstream os;\n"
      "  if (obj->vtk_ptr)\n"
      "    {\n"
      "    os << *static_cast<const %s *>(obj->vtk_ptr);\n"
      "    }\n"
      "  const vtksys_stl::string &s = os.str();\n"
      "  return PyString_FromStringAndSize(s.data(), s.size());\n"
      "}\n"
      "\n",
      classname, data->Name);
    }
}

/* -------------------------------------------------------------------- */
/* generate function for comparing special objects */
static void vtkWrapPython_RichCompareProtocol(
  FILE *fp, const char *classname, ClassInfo *data,
  FileInfo *finfo, SpecialTypeInfo *info)
{
  static const char *compare_consts[6] = {
    "Py_LT", "Py_LE", "Py_EQ", "Py_NE", "Py_GT", "Py_GE" };
  static const char *compare_tokens[6] = {
    "<", "<=", "==", "!=", ">", ">=" };
  int compare_ops = 0;
  int i, n;
  FunctionInfo *func;

  /* look for comparison operator methods */
  n = data->NumberOfFunctions + finfo->Contents->NumberOfFunctions;
  for (i = 0; i < n; i++)
    {
    if (i < data->NumberOfFunctions)
      {
      /* member function */
      func = data->Functions[i];
      if (func->NumberOfParameters != 1 ||
          (func->Parameters[0]->Type & VTK_PARSE_BASE_TYPE) !=
              VTK_PARSE_OBJECT ||
          (func->Parameters[0]->Type & VTK_PARSE_POINTER_MASK) != 0 ||
          strcmp(func->Parameters[0]->Class, data->Name) != 0)
        {
        continue;
        }
      }
    else
      {
      /* non-member function: both args must be of our type */
      func = finfo->Contents->Functions[i - data->NumberOfFunctions];
      if (func->NumberOfParameters != 2 ||
          (func->Parameters[0]->Type & VTK_PARSE_BASE_TYPE) !=
              VTK_PARSE_OBJECT ||
          (func->Parameters[0]->Type & VTK_PARSE_POINTER_MASK) != 0 ||
          strcmp(func->Parameters[0]->Class, data->Name) != 0 ||
          (func->Parameters[1]->Type & VTK_PARSE_BASE_TYPE) !=
              VTK_PARSE_OBJECT ||
          (func->Parameters[1]->Type & VTK_PARSE_POINTER_MASK) != 0 ||
          strcmp(func->Parameters[1]->Class, data->Name) != 0)
        {
        continue;
        }
      }
    if (func->IsOperator && func->Name != NULL)
      {
      if (strcmp(func->Name, "operator<") == 0)
        {
        compare_ops = (compare_ops | (1 << 0));
        }
      else if (strcmp(func->Name, "operator<=") == 0)
        {
        compare_ops = (compare_ops | (1 << 1));
        }
      else if (strcmp(func->Name, "operator==") == 0)
        {
        compare_ops = (compare_ops | (1 << 2));
        }
      else if (strcmp(func->Name, "operator!=") == 0)
        {
        compare_ops = (compare_ops | (1 << 3));
        }
      else if (strcmp(func->Name, "operator>") == 0)
        {
        compare_ops = (compare_ops | (1 << 4));
        }
      else if (strcmp(func->Name, "operator>=") == 0)
        {
        compare_ops = (compare_ops | (1 << 5));
        }
      }
    }

  /* the compare function */
  if (compare_ops != 0)
    {
    info->has_compare = 1;

    fprintf(fp,
      "#if PY_VERSION_HEX >= 0x02010000\n"
      "static PyObject *Py%s_RichCompare(\n"
      "  PyObject *o1, PyObject *o2, int opid)\n"
      "{\n"
      "  PyObject *n1 = NULL;\n"
      "  PyObject *n2 = NULL;\n"
      "  const %s *so1 = NULL;\n"
      "  const %s *so2 = NULL;\n"
      "  int result = -1;\n"
      "\n",
      classname, data->Name, data->Name);

    for (i = 1; i <= 2; i++)
      {
      /* use GetPointerFromSpecialObject to do type conversion, but
       * at least one of the args will already be the correct type */
      fprintf(fp,
        "  if (o%d->ob_type == &Py%s_Type)\n"
        "    {\n"
        "    PyVTKSpecialObject *s%d = (PyVTKSpecialObject *)o%d;\n"
        "    so%d = static_cast<const %s *>(s%d->vtk_ptr);\n"
        "    }\n"
        "  else\n"
        "    {\n"
        "    so%d = static_cast<const %s *>(\n"
        "      vtkPythonUtil::GetPointerFromSpecialObject(\n"
        "        o%d, \"%s\", &n%d));\n"
        "    if (so%d == NULL)\n"
        "      {\n"
        "      PyErr_Clear();\n"
        "      Py_INCREF(Py_NotImplemented);\n"
        "      return Py_NotImplemented;\n"
        "      }\n"
        "    }\n"
        "\n",
        i, classname, i, i, i, data->Name, i, i, data->Name,
        i, classname, i, i);
      }

    /* the switch statement for all possible compare ops */
    fprintf(fp,
      "  switch (opid)\n"
      "    {\n");

    for (i = 0; i < 6; i++)
      {
      if ( ((compare_ops >> i) & 1) != 0 )
        {
        fprintf(fp,
          "    case %s:\n"
          "      result = ((*so1) %s (*so2));\n"
          "      break;\n",
          compare_consts[i], compare_tokens[i]);
        }
      else
        {
        fprintf(fp,
          "    case %s:\n"
          "      break;\n",
          compare_consts[i]);
        }
      }

    fprintf(fp,
      "    }\n"
      "\n");

    /* delete temporary objects, there will be at most one */
    fprintf(fp,
      "  if (n1)\n"
      "    {\n"
      "    Py_DECREF(n1);\n"
      "    }\n"
      "  else if (n2)\n"
      "    {\n"
      "    Py_DECREF(n2);\n"
      "    }\n"
      "\n");

    /* return the result */
    fprintf(fp,
      "  if (result == -1)\n"
      "    {\n"
      "    PyErr_SetString(PyExc_TypeError, (char *)\"operation not available\");\n"
      "    return NULL;\n"
      "    }\n"
      "\n"
      "#if PY_VERSION_HEX >= 0x02030000\n"
      "  // avoids aliasing issues with Py_INCREF(Py_False)\n"
      "  return PyBool_FromLong((long)result);\n"
      "#else\n"
      "  if (result == 0)\n"
      "    {\n"
      "    Py_INCREF(Py_False);\n"
      "    return Py_False;\n"
      "    }\n"
      "  Py_INCREF(Py_True);\n"
      "  return Py_True;\n"
      "#endif\n"
      "}\n"
      "#endif\n"
      "\n");
    }
}

/* -------------------------------------------------------------------- */
/* generate functions for indexing into special objects */
static void vtkWrapPython_SequenceProtocol(
  FILE *fp, const char *classname, ClassInfo *data,
  HierarchyInfo *hinfo, SpecialTypeInfo *info)
{
  int i;
  FunctionInfo *func;
  FunctionInfo *getItemFunc = 0;
  FunctionInfo *setItemFunc = 0;

  /* look for [] operator */
  for (i = 0; i < data->NumberOfFunctions; i++)
    {
    func = data->Functions[i];

    if (func->Name && func->IsOperator &&
        strcmp(func->Name, "operator[]") == 0  &&
        vtkWrapPython_MethodCheck(func, hinfo))
      {
      if (func->NumberOfParameters == 1 && func->ReturnValue &&
          vtkWrap_IsInteger(func->Parameters[0]))
        {
        if (!setItemFunc && vtkWrap_IsNonConstRef(func->ReturnValue))
          {
          setItemFunc = func;
          }
        if (!getItemFunc || (func->IsConst && !getItemFunc->IsConst))
          {
          getItemFunc = func;
          }
        }
      }
    }

  if (getItemFunc && getItemFunc->SizeHint)
    {
    info->has_sequence = 1;

    fprintf(fp,
      "Py_ssize_t Py%s_SequenceSize(PyObject *self)\n"
      "{\n"
      "  void *vp = vtkPythonArgs::GetSelfPointer(self);\n"
      "  %s *op = static_cast<%s *>(vp);\n"
      "\n"
      "  return static_cast<Py_ssize_t>(op->%s);\n"
      "}\n\n",
      classname, data->Name, data->Name, getItemFunc->SizeHint);

    fprintf(fp,
      "PyObject *Py%s_SequenceItem(PyObject *self, Py_ssize_t i)\n"
      "{\n"
      "  void *vp = vtkPythonArgs::GetSelfPointer(self);\n"
      "  %s *op = static_cast<%s *>(vp);\n"
      "\n",
      classname, data->Name, data->Name);

    vtkWrapPython_DeclareVariables(fp, getItemFunc);

    fprintf(fp,
            "  temp0 = static_cast<%s>(i);\n"
            "\n"
            "  if (temp0 < 0 || temp0 >= op->%s)\n"
            "    {\n"
            "    PyErr_SetString(PyExc_IndexError, \"index out of range\");\n"
            "    }\n"
            "  else\n"
            "    {\n",
            vtkWrap_GetTypeName(getItemFunc->Parameters[0]),
            getItemFunc->SizeHint);

    fprintf(fp, "  ");
    vtkWrap_DeclareVariable(fp, getItemFunc->ReturnValue,
      "tempr", -1, VTK_WRAP_RETURN | VTK_WRAP_NOSEMI);

    fprintf(fp, " = %s(*op)[temp0];\n"
            "\n",
            (vtkWrap_IsRef(getItemFunc->ReturnValue) ? "&" : ""));

    vtkWrapPython_ReturnValue(fp, getItemFunc->ReturnValue, 1);

    fprintf(fp,
            "    }\n"
            "\n"
            "  return result;\n"
            "}\n\n");

    if (setItemFunc)
      {
      fprintf(fp,
        "int Py%s_SequenceSetItem(\n"
        "  PyObject *self, Py_ssize_t i, PyObject *arg1)\n"
        "{\n"
        "  void *vp = vtkPythonArgs::GetSelfPointer(self);\n"
        "  %s *op = static_cast<%s *>(vp);\n"
        "\n",
        classname, data->Name, data->Name);

      vtkWrap_DeclareVariable(fp, setItemFunc->Parameters[0], "temp", 0,
                              VTK_WRAP_ARG);
      vtkWrap_DeclareVariable(fp, setItemFunc->ReturnValue, "temp", 1,
                              VTK_WRAP_ARG);

      fprintf(fp,
              "  int result = -1;\n"
              "\n"
              "  temp0 = static_cast<%s>(i);\n"
              "\n"
              "  if (temp0 < 0 || temp0 >= op->%s)\n"
              "    {\n"
              "    PyErr_SetString(PyExc_IndexError, \"index out of range\");\n"
              "    }\n"
              "  else if (",
              vtkWrap_GetTypeName(setItemFunc->Parameters[0]),
              getItemFunc->SizeHint);

      vtkWrapPython_GetSingleArgument(fp, 1, setItemFunc->ReturnValue, 1);

      fprintf(fp,")\n"
              "    {\n"
              "    (*op)[temp0] = %stemp1;\n"
              "\n",
              ((vtkWrap_IsRef(getItemFunc->ReturnValue) &&
                vtkWrap_IsObject(getItemFunc->ReturnValue)) ? "*" : ""));

      fprintf(fp,
              "    if (PyErr_Occurred() == NULL)\n"
              "      {\n"
              "      result = 0;\n"
              "      }\n"
              "    }\n"
              "\n"
              "  return result;\n"
              "}\n\n");
      }

    fprintf(fp,
      "static PySequenceMethods Py%s_AsSequence = {\n"
      "  Py%s_SequenceSize, // sq_length\n"
      "  0, // sq_concat\n"
      "  0, // sq_repeat\n"
      "  Py%s_SequenceItem, // sq_item\n"
      "  0, // sq_slice\n",
      classname, classname, classname);

    if (setItemFunc)
      {
      fprintf(fp,
        "  Py%s_SequenceSetItem, // sq_ass_item\n",
        classname);
      }
    else
      {
      fprintf(fp,
        "  0, // sq_ass_item\n");
      }

    fprintf(fp,
      "  0, // sq_ass_slice\n"
      "  0, // sq_contains\n"
      "#if PY_VERSION_HEX >= 0x2000000\n"
      "  0, // sq_inplace_concat\n"
      "  0, // sq_inplace_repeat\n"
      "#endif\n"
      "};\n\n");
    }
}

/* -------------------------------------------------------------------- */
/* generate function for hashing special objects */
static void vtkWrapPython_HashProtocol(
  FILE *fp, const char *classname, ClassInfo *data)
{
  /* the hash function, defined only for specific types */
  fprintf(fp,
    "static long Py%s_Hash(PyObject *self)\n",
    classname);

  if (strcmp(data->Name, "vtkTimeStamp") == 0)
    {
    /* hash for vtkTimeStamp is just the timestamp itself */
    fprintf(fp,
      "{\n"
      "  PyVTKSpecialObject *obj = (PyVTKSpecialObject *)self;\n"
      "  const vtkTimeStamp *op = static_cast<const vtkTimeStamp *>(obj->vtk_ptr);\n"
      "  unsigned long mtime = *op;\n"
      "  long h = (long)mtime;\n"
      "  if (h != -1) { return h; }\n"
      "  return -2;\n"
      "}\n"
      "\n");
    }
  else if (strcmp(data->Name, "vtkVariant") == 0)
    {
    /* hash for vtkVariant is cached to avoid recomputation, this is
     * safe because vtkVariant is an immutable object, and is necessary
     * because computing the hash for vtkVariant is very expensive */
    fprintf(fp,
      "{\n"
      "  PyVTKSpecialObject *obj = (PyVTKSpecialObject *)self;\n"
      "  const vtkVariant *op = static_cast<const vtkVariant *>(obj->vtk_ptr);\n"
      "  long h = obj->vtk_hash;\n"
      "  if (h != -1)\n"
      "    {\n"
      "    return h;\n"
      "    }\n"
      "  h = vtkPythonUtil::VariantHash(op);\n"
      "  obj->vtk_hash = h;\n"
      "  return h;\n"
      "}\n"
      "\n");
    }
  else
    {
    /* if hash is not implemented, raise an exception */
    fprintf(fp,
      "{\n"
      "#if PY_VERSION_HEX >= 0x020600B2\n"
      "  return PyObject_HashNotImplemented(self);\n"
      "#else\n"
      "  char text[256];\n"
      "  sprintf(text, \"unhashable type: \'%%s\'\", self->ob_type->tp_name);\n"
      "  PyErr_SetString(PyExc_TypeError, text);\n"
      "  return -1;\n"
      "#endif\n"
      "}\n"
      "\n");
    }

}

/* -------------------------------------------------------------------- */
/* generate extra functions for a special object */
static void vtkWrapPython_SpecialTypeProtocols(
  FILE *fp, const char *classname, ClassInfo *data,
  FileInfo *finfo, HierarchyInfo *hinfo, SpecialTypeInfo *info)
{
  /* clear all info about the type */
  info->has_print = 0;
  info->has_compare = 0;
  info->has_sequence = 0;

  vtkWrapPython_NewDeleteProtocol(fp, classname, data);
  vtkWrapPython_PrintProtocol(fp, classname, data, finfo, info);
  vtkWrapPython_RichCompareProtocol(fp, classname, data, finfo, info);
  vtkWrapPython_SequenceProtocol(fp, classname, data, hinfo, info);
  vtkWrapPython_HashProtocol(fp, classname, data);
}

/* -------------------------------------------------------------------- */
/* For classes that aren't derived from vtkObjectBase, check to see if
 * they are wrappable */
int vtkWrapPython_IsSpecialTypeWrappable(ClassInfo *data)
{
  /* no templated types */
  if (data->Template)
    {
    return 0;
    }

  /* no abstract classes */
  if (data->IsAbstract)
    {
    return 0;
    }

  if (strncmp(data->Name, "vtk", 3) != 0)
    {
    return 0;
    }

  /* require public destructor and copy contructor */
  if (!vtkWrap_HasPublicDestructor(data) ||
      !vtkWrap_HasPublicCopyConstructor(data))
    {
    return 0;
    }

  return 1;
}

/* -------------------------------------------------------------------- */
/* write out a special type object */
void vtkWrapPython_GenerateSpecialType(
  FILE *fp, const char *classname, ClassInfo *data,
  FileInfo *finfo, HierarchyInfo *hinfo)
{
  char supername[1024];
  const char *name;
  SpecialTypeInfo info;
  const char *constructor;
  size_t n, m;
  int has_superclass = 0;
  int is_external = 0;

  /* remove namespaces and template parameters from the
   * class name to get the constructor name */
  constructor = data->Name;
  m = vtkParse_UnscopedNameLength(constructor);
  while (constructor[m] == ':' && constructor[m+1] == ':')
    {
    constructor += m + 2;
    m = vtkParse_UnscopedNameLength(constructor);
    }
  for (n = 0; n < m; n++)
    {
    if (constructor[n] == '<')
      {
      break;
      }
    }

  /* forward declaration of the type object */
  fprintf(fp,
    "#ifndef DECLARED_Py%s_Type\n"
    "extern %s PyTypeObject Py%s_Type;\n"
    "#define DECLARED_Py%s_Type\n"
    "#endif\n"
    "\n",
    classname, "VTK_ABI_EXPORT", classname, classname);

  /* and the superclass */
  has_superclass = vtkWrapPython_HasWrappedSuperClass(
    hinfo, data->Name, &is_external);
  if (has_superclass)
    {
    name = vtkWrapPython_GetSuperClass(data, hinfo);
    vtkWrapPython_PythonicName(name, supername);
    fprintf(fp,
      "#ifndef DECLARED_Py%s_Type\n"
      "extern %s PyTypeObject Py%s_Type;\n"
      "#define DECLARED_Py%s_Type\n"
      "#endif\n"
      "\n",
#if defined(VTK_BUILD_SHARED_LIBS)
      supername, (is_external ? "VTK_ABI_IMPORT" : "VTK_ABI_EXPORT"),
#else
      supername, "VTK_ABI_EXPORT",
#endif
      supername, supername);
    }

  /* generate all constructor methods */
  vtkWrapPython_GenerateMethods(fp, classname, data, finfo, hinfo, 0, 1);

  /* generate the method table for the New method */
  fprintf(fp,
    "static PyMethodDef Py%s_NewMethod = \\\n"
    "{ (char*)\"%s\", Py%s_%*.*s, 1,\n"
    "  (char*)\"\" };\n"
    "\n",
    classname, classname, classname,
    (int)n, (int)n, constructor);

  /* generate all functions and protocols needed for the type */
  vtkWrapPython_SpecialTypeProtocols(
    fp, classname, data, finfo, hinfo, &info);

  /* Generate the TypeObject */
  fprintf(fp,
    "PyTypeObject Py%s_Type = {\n"
    "  PyObject_HEAD_INIT(&PyType_Type)\n"
    "  0,\n"
    "  (char*)\"%s\", // tp_name\n"
    "  sizeof(PyVTKSpecialObject), // tp_basicsize\n"
    "  0, // tp_itemsize\n"
    "  Py%s_Delete, // tp_dealloc\n"
    "  0, // tp_print\n"
    "  0, // tp_getattr\n"
    "  0, // tp_setattr\n"
    "  0, // tp_compare\n"
    "  PyVTKSpecialObject_Repr, // tp_repr\n",
    classname, classname, classname);

  fprintf(fp,
    "  0, // tp_as_number\n");

  if (info.has_sequence)
    {
    fprintf(fp,
      "  &Py%s_AsSequence, // tp_as_sequence\n",
    classname);
    }
  else
    {
  fprintf(fp,
      "  0, // tp_as_sequence\n");
    }

  fprintf(fp,
    "  0, // tp_as_mapping\n"
    "  Py%s_Hash, // tp_hash\n"
    "  0, // tp_call\n",
    classname);

  if (info.has_print)
    {
    fprintf(fp,
      "  Py%s_String, // tp_str\n",
      classname);
    }
  else if (info.has_sequence)
    {
    fprintf(fp,
      "  PyVTKSpecialObject_SequenceString, // tp_str\n");
    }
  else
    {
    fprintf(fp,
      "  0, // tp_str\n");
    }

  fprintf(fp,
    "#if PY_VERSION_HEX >= 0x02020000\n"
    "  PyObject_GenericGetAttr, // tp_getattro\n"
    "#else\n"
    "  PyVTKSpecialObject_GetAttr, // tp_getattro\n"
    "#endif\n"
    "  0, // tp_setattro\n"
    "  0, // tp_as_buffer\n"
    "  Py_TPFLAGS_DEFAULT, // tp_flags\n"
    "  0, // tp_doc\n"
    "  0, // tp_traverse\n"
    "  0, // tp_clear\n");

  if (info.has_compare)
    {
    fprintf(fp,
      "#if PY_VERSION_HEX >= 0x02010000\n"
      "  Py%s_RichCompare, // tp_richcompare\n"
      "#else\n"
      "  0, // tp_richcompare\n"
      "#endif\n",
      classname);
    }
  else
    {
    fprintf(fp,
      "  0, // tp_richcompare\n");
    }

  fprintf(fp,
    "  0, // tp_weaklistoffset\n"
    "#if PY_VERSION_HEX >= 0x02020000\n"
    "  0, // tp_iter\n"
    "  0, // tp_iternext\n");

  /* class methods introduced in python 2.2 */
  fprintf(fp,
    "  Py%s_Methods, // tp_methods\n"
    "  0, // tp_members\n"
    "  0, // tp_getset\n",
    classname);

  if (has_superclass)
    {
    fprintf(fp,
      "  &Py%s_Type, // tp_base\n",
      supername);
    }
  else
    {
    fprintf(fp,
      "  0, // tp_base\n");
    }

  fprintf(fp,
    "  0, // tp_dict\n"
    "  0, // tp_descr_get\n"
    "  0, // tp_descr_set\n"
    "  0, // tp_dictoffset\n"
    "  0, // tp_init\n"
    "  0, // tp_alloc\n"
    "  Py%s_New, // tp_new\n"
    "#if PY_VERSION_HEX >= 0x02030000\n"
    "  PyObject_Del, // tp_free\n"
    "#else\n"
    "  _PyObject_Del, // tp_free\n"
    "#endif\n"
    "  0, // tp_is_gc\n",
    classname);

  /* fields set by python itself */
  fprintf(fp,
    "  0, // tp_bases\n"
    "  0, // tp_mro\n"
    "  0, // tp_cache\n"
    "  0, // tp_subclasses\n"
    "  0, // tp_weaklist\n"
    "#endif\n");

  /* internal struct members */
  fprintf(fp,
    "  VTK_WRAP_PYTHON_SUPRESS_UNINITIALIZED\n"
    "};\n"
    "\n");

  /* generate the copy constructor helper function */
  fprintf(fp,
    "static void *Py%s_CCopy(const void *obj)\n"
    "{\n"
    "  if (obj)\n"
    "    {\n"
    "    return new %s(*static_cast<const %s*>(obj));\n"
    "    }\n"
    "  return 0;\n"
    "}\n"
    "\n",
    classname, data->Name, data->Name);

  /* the method for adding the VTK extras to the type,
   * the unused "const char *" arg is the module name */
  fprintf(fp,
    "static PyObject *Py%s_TypeNew(const char *)\n"
    "{\n"
    "  return PyVTKSpecialType_New(\n"
    "    &Py%s_Type,\n"
    "    Py%s_Methods,\n"
    "    Py%s_%*.*s_Methods,\n"
    "    &Py%s_NewMethod,\n"
    "    Py%s_Doc(), &Py%s_CCopy);\n"
    "}\n"
    "\n",
    classname, classname, classname,
    classname, (int)n, (int)n, constructor, classname,
    classname, classname);
}
