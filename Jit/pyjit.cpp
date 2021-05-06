// Copyright (c) Facebook, Inc. and its affiliates. (http://www.facebook.com)
#include "Jit/pyjit.h"

#include "Python.h"

#include "Include/internal/pycore_pystate.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#include <memory>
#include <thread>
#include <unordered_set>

#include "Jit/hir/builder.h"
#include "Jit/jit_context.h"
#include "Jit/jit_gdb_support.h"
#include "Jit/jit_list.h"
#include "Jit/jit_x_options.h"
#include "Jit/log.h"
#include "Jit/perf_jitdump.h"
#include "Jit/ref.h"
#include "Jit/runtime.h"

#define DEFAULT_CODE_SIZE 2 * 1024 * 1024

using namespace jit;

enum InitState { JIT_NOT_INITIALIZED, JIT_INITIALIZED, JIT_FINALIZED };
enum FrameMode { PY_FRAME = 0, TINY_FRAME, NO_FRAME };

struct JitConfig {
  InitState init_state{JIT_NOT_INITIALIZED};

  int is_enabled{0};
  FrameMode frame_mode{PY_FRAME};
  int are_type_slots_enabled{0};
  int allow_jit_list_wildcards{0};
  int compile_all_static_functions{0};
  size_t batch_compile_workers{0};
  int test_multithreaded_compile{0};
};
JitConfig jit_config;

static _PyJITContext* jit_ctx;
static JITList* g_jit_list{nullptr};
static std::unordered_set<PyFunctionObject*> jit_reg_functions;
static std::vector<Ref<PyFunctionObject>> test_multithreaded_funcs;
static std::unordered_map<PyFunctionObject*, std::chrono::duration<double>>
    jit_time_functions;

static double total_compliation_time = 0.0;

struct CompilationTimer {
  explicit CompilationTimer(PyFunctionObject* f)
      : start(std::chrono::steady_clock::now()), func(f) {}

  ~CompilationTimer() {
    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> time_span =
        std::chrono::duration_cast<std::chrono::duration<double>>(end - start);

    double time = time_span.count();
    total_compliation_time += time;
    jit_time_functions.emplace(func, time_span);
  }

  std::chrono::steady_clock::time_point start;
  PyFunctionObject* func{nullptr};
};

static std::atomic<int> g_compile_workers_attempted;
static int g_compile_workers_retries;

static void compile_worker_thread() {
  JIT_DLOG("Started compile worker in thread %d", std::this_thread::get_id());
  PyFunctionObject* func;
  while ((func = g_threaded_compile_context.nextFunction()) != nullptr) {
    CompilationTimer t{func};
    // The list of conditions here should be matched in _PyJIT_CompileFunction
    {
      ThreadedCompileSerialize guard;
      if ((!jit_config.test_multithreaded_compile &&
           _PyJIT_IsCompiled(reinterpret_cast<PyObject*>(func))) ||
          !_PyJIT_OnJitList(func)) {
        continue;
      }
    }
    g_compile_workers_attempted++;
    if (_PyJITContext_CompileFunction(jit_ctx, func) == PYJIT_RESULT_RETRY) {
      ThreadedCompileSerialize guard;
      g_compile_workers_retries++;
      g_threaded_compile_context.retryFunction(func);
      JIT_LOG("Retrying compile of function: %s", funcFullname(func));
    }
  }
  JIT_DLOG("Finished compile worker in thread %d", std::this_thread::get_id());
}

static void multithread_compile_all() {
  JIT_CHECK(jit_ctx, "JIT not initialized");

  // Disable checks for using GIL protected data across threads.
  // Conceptually what we're doing here is saying we're taking our own
  // responsibility for managing locking of CPython runtime data structures.
  // Instead of holding the GIL to serialize execution to one thread, we're
  // holding the GIL for a group of co-operating threads which are aware of each
  // other. We still need the GIL as this protects the cooperating threads from
  // unknown other threads. Within our group of cooperating threads we can
  // safely do any read-only operations in parallel, but we grab our own lock if
  // we do a write (e.g. an incref).
  int old_gil_check_enabled = _PyGILState_check_enabled;
  _PyGILState_check_enabled = 0;

  g_threaded_compile_context.startCompile(
      {jit_reg_functions.begin(), jit_reg_functions.end()});
  jit_reg_functions.clear();
  std::vector<std::thread> worker_threads;
  JIT_CHECK(jit_config.batch_compile_workers, "Zero workers for compile");
  {
    // Hold a lock while we create threads because IG production has magic to
    // wrap pthread_create() and run Python code before threads are created.
    ThreadedCompileSerialize guard;

    for (size_t i = 0; i < jit_config.batch_compile_workers; i++) {
      worker_threads.emplace_back(compile_worker_thread);
    }
  }
  for (std::thread& worker_thread : worker_threads) {
    worker_thread.join();
  }
  std::vector<PyFunctionObject*> retry_list{
      g_threaded_compile_context.endCompile()};
  for (PyFunctionObject* func : retry_list) {
    _PyJIT_CompileFunction(func);
  }
  _PyGILState_check_enabled = old_gil_check_enabled;
}

static PyObject* test_multithreaded_compile(PyObject*, PyObject*) {
  if (!jit_config.test_multithreaded_compile) {
    PyErr_SetString(
        PyExc_NotImplementedError, "test_multithreaded_compile not enabled");
    return NULL;
  }
  std::unordered_set<PyFunctionObject*> jit_reg_funcs_copy(jit_reg_functions);
  jit_reg_functions.clear();
  for (PyFunctionObject* func : test_multithreaded_funcs) {
    jit_reg_functions.insert(func);
  }
  g_compile_workers_attempted = 0;
  g_compile_workers_retries = 0;
  JIT_LOG("(Re)compiling %d functions", jit_reg_functions.size());
  std::chrono::time_point time_start = std::chrono::steady_clock::now();
  multithread_compile_all();
  std::chrono::time_point time_end = std::chrono::steady_clock::now();
  JIT_LOG(
      "Took %d ms, compiles attempted: %d, compiles retried: %d",
      std::chrono::duration_cast<std::chrono::milliseconds>(
          time_end - time_start)
          .count(),
      g_compile_workers_attempted,
      g_compile_workers_retries);
  std::unordered_set<PyFunctionObject*> func_copy(jit_reg_functions);
  for (PyFunctionObject* func : func_copy) {
    jit_reg_functions.erase(func);
  }
  jit_reg_functions = jit_reg_funcs_copy;
  test_multithreaded_funcs.clear();
  Py_RETURN_NONE;
}

static PyObject* is_test_multithreaded_compile_enabled(PyObject*, PyObject*) {
  if (jit_config.test_multithreaded_compile) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

static PyObject*
disable_jit(PyObject* /* self */, PyObject* const* args, Py_ssize_t nargs) {
  if (nargs > 1) {
    PyErr_SetString(PyExc_TypeError, "disable expects 0 or 1 arg");
    return NULL;
  } else if (nargs == 1 && !PyBool_Check(args[0])) {
    PyErr_SetString(
        PyExc_TypeError,
        "disable expects bool indicating to compile pending functions");
    return NULL;
  }

  if (nargs == 0 || args[0] == Py_True) {
    // Compile all pending functions before shutting down.
    if (jit_config.batch_compile_workers > 0) {
      multithread_compile_all();
    } else {
      std::unordered_set<PyFunctionObject*> func_copy(jit_reg_functions);
      for (PyFunctionObject* func : func_copy) {
        _PyJIT_CompileFunction(func);
      }
    }
  }

  _PyJIT_Disable();
  Py_RETURN_NONE;
}

static PyObject* force_compile(PyObject* /* self */, PyObject* func) {
  if (!PyFunction_Check(func)) {
    PyErr_SetString(PyExc_TypeError, "force_compile expected a function");
    return NULL;
  }

  if (jit_reg_functions.find((PyFunctionObject*)func) !=
      jit_reg_functions.end()) {
    _PyJIT_CompileFunction((PyFunctionObject*)func);
    Py_RETURN_TRUE;
  }

  Py_RETURN_FALSE;
}

int _PyJIT_IsCompiled(PyObject* func) {
  if (jit_ctx == nullptr) {
    return 0;
  }
  if (!PyFunction_Check(func)) {
    return 0;
  }

  return _PyJITContext_DidCompile(jit_ctx, func);
}

static PyObject* is_jit_compiled(PyObject* /* self */, PyObject* func) {
  int st = _PyJIT_IsCompiled(func);
  PyObject* res = NULL;
  if (st == 1) {
    res = Py_True;
  } else if (st == 0) {
    res = Py_False;
  }
  Py_XINCREF(res);
  return res;
}

static PyObject* print_hir(PyObject* /* self */, PyObject* func) {
  if (!PyFunction_Check(func)) {
    PyErr_SetString(PyExc_TypeError, "arg 1 must be a function");
    return NULL;
  }

  int st = _PyJITContext_DidCompile(jit_ctx, func);
  if (st == -1) {
    return NULL;
  } else if (st == 0) {
    PyErr_SetString(PyExc_ValueError, "function is not jit compiled");
    return NULL;
  }

  if (_PyJITContext_PrintHIR(jit_ctx, func) < 0) {
    return NULL;
  } else {
    Py_RETURN_NONE;
  }
}

static PyObject* disassemble(PyObject* /* self */, PyObject* func) {
  if (!PyFunction_Check(func)) {
    PyErr_SetString(PyExc_TypeError, "arg 1 must be a function");
    return NULL;
  }

  int st = _PyJITContext_DidCompile(jit_ctx, func);
  if (st == -1) {
    return NULL;
  } else if (st == 0) {
    PyErr_SetString(PyExc_ValueError, "function is not jit compiled");
    return NULL;
  }

  if (_PyJITContext_Disassemble(jit_ctx, func) < 0) {
    return NULL;
  } else {
    Py_RETURN_NONE;
  }
}

static PyObject* get_jit_list(PyObject* /* self */, PyObject*) {
  if (g_jit_list == nullptr) {
    Py_RETURN_NONE;
  } else {
    auto jit_list = Ref<>::steal(g_jit_list->getList());
    return jit_list.release();
  }
}

static PyObject* get_compiled_functions(PyObject* /* self */, PyObject*) {
  return _PyJITContext_GetCompiledFunctions(jit_ctx);
}

static PyObject* get_compilation_time(PyObject* /* self */, PyObject*) {
  PyObject* res =
      PyLong_FromLong(static_cast<long>(total_compliation_time * 1000));
  return res;
}

static PyObject* get_function_compilation_time(
    PyObject* /* self */,
    PyObject* func) {
  auto iter =
      jit_time_functions.find(reinterpret_cast<PyFunctionObject*>(func));
  if (iter == jit_time_functions.end()) {
    Py_RETURN_NONE;
  }

  PyObject* res = PyLong_FromLong(iter->second.count() * 1000);
  return res;
}

static PyObject* get_compiled_size(PyObject* /* self */, PyObject* func) {
  if (jit_ctx == NULL) {
    return PyLong_FromLong(0);
  }

  long size = _PyJITContext_GetCodeSize(jit_ctx, func);
  PyObject* res = PyLong_FromLong(size);
  return res;
}

static PyObject* get_compiled_stack_size(PyObject* /* self */, PyObject* func) {
  if (jit_ctx == NULL) {
    return PyLong_FromLong(0);
  }

  long size = _PyJITContext_GetStackSize(jit_ctx, func);
  PyObject* res = PyLong_FromLong(size);
  return res;
}

static PyObject* get_compiled_spill_stack_size(
    PyObject* /* self */,
    PyObject* func) {
  if (jit_ctx == NULL) {
    return PyLong_FromLong(0);
  }

  long size = _PyJITContext_GetSpillStackSize(jit_ctx, func);
  PyObject* res = PyLong_FromLong(size);
  return res;
}

static PyObject* jit_frame_mode(PyObject* /* self */, PyObject*) {
  int i = PY_FRAME;
  if (_PyJIT_TinyFrame()) {
    i = TINY_FRAME;
  } else if (_PyJIT_NoFrame()) {
    i = NO_FRAME;
  }

  return PyLong_FromLong(i);
}

static PyObject* get_supported_opcodes(PyObject* /* self */, PyObject*) {
  auto set = Ref<>::steal(PySet_New(nullptr));
  if (set == nullptr) {
    return nullptr;
  }

  for (auto op : hir::kSupportedOpcodes) {
    auto op_obj = Ref<>::steal(PyLong_FromLong(op));
    if (op_obj == nullptr) {
      return nullptr;
    }
    if (PySet_Add(set, op_obj) < 0) {
      return nullptr;
    }
  }

  return set.release();
}

static PyObject* jit_force_normal_frame(PyObject*, PyObject* func_obj) {
  if (!PyFunction_Check(func_obj)) {
    PyErr_SetString(PyExc_TypeError, "Input must be a function");
    return NULL;
  }
  PyFunctionObject* func = reinterpret_cast<PyFunctionObject*>(func_obj);

  reinterpret_cast<PyCodeObject*>(func->func_code)->co_flags |= CO_NORMAL_FRAME;

  Py_INCREF(func_obj);
  return func_obj;
}

static PyMethodDef jit_methods[] = {
    {"disable",
     (PyCFunction)(void*)disable_jit,
     METH_FASTCALL,
     "Disable the jit."},
    {"disassemble", disassemble, METH_O, "Disassemble JIT compiled functions"},
    {"is_jit_compiled",
     is_jit_compiled,
     METH_O,
     "Check if a function is jit compiled."},
    {"force_compile",
     force_compile,
     METH_O,
     "Force a function to be JIT compiled if it hasn't yet"},
    {"jit_frame_mode",
     jit_frame_mode,
     METH_NOARGS,
     "Get JIT frame mode (0 = normal frames, 1 = tiny frames, 2 = no frames"},
    {"get_jit_list", get_jit_list, METH_NOARGS, "Get the JIT-list"},
    {"print_hir",
     print_hir,
     METH_O,
     "Print the HIR for a jitted function to stdout."},
    {"get_supported_opcodes",
     get_supported_opcodes,
     METH_NOARGS,
     "Return a set of all supported opcodes, as ints."},
    {"get_compiled_functions",
     get_compiled_functions,
     METH_NOARGS,
     "Return a list of functions that are currently JIT-compiled."},
    {"get_compilation_time",
     get_compilation_time,
     METH_NOARGS,
     "Return the total time used for JIT compiling functions in milliseconds."},
    {"get_function_compilation_time",
     get_function_compilation_time,
     METH_O,
     "Return the time used for JIT compiling a given function in "
     "milliseconds."},
    {"get_compiled_size",
     get_compiled_size,
     METH_O,
     "Return code size in bytes for a JIT-compiled function."},
    {"get_compiled_stack_size",
     get_compiled_stack_size,
     METH_O,
     "Return stack size in bytes for a JIT-compiled function."},
    {"get_compiled_spill_stack_size",
     get_compiled_spill_stack_size,
     METH_O,
     "Return stack size in bytes used for register spills for a JIT-compiled "
     "function."},
    {"jit_force_normal_frame",
     jit_force_normal_frame,
     METH_O,
     "Decorator forcing a function to always use normal frame mode when JIT."},
    {"test_multithreaded_compile",
     test_multithreaded_compile,
     METH_NOARGS,
     "Force multi-threaded recompile of still existing JIT functions for test"},
    {"is_test_multithreaded_compile_enabled",
     is_test_multithreaded_compile_enabled,
     METH_NOARGS,
     "Return True if test_multithreaded_compile mode is enabled"},
    {NULL, NULL, 0, NULL}};

static PyModuleDef jit_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "cinderjit",
    .m_doc = NULL,
    .m_size = -1,
    .m_methods = jit_methods,
    .m_slots = nullptr,
    .m_traverse = nullptr,
    .m_clear = nullptr,
    .m_free = nullptr};

int _PyJIT_OnJitList(PyFunctionObject* func) {
  BorrowedRef<PyCodeObject> code(func->func_code);
  bool is_static = code->co_flags & CO_STATICALLY_COMPILED;
  if (g_jit_list == nullptr ||
      (is_static && jit_config.compile_all_static_functions)) {
    // There's no jit list or the function is static.
    return 1;
  }
  return g_jit_list->lookup(func);
}

// Is env var set to a value other than "0" or ""?
int _is_env_truthy(const char* name) {
  const char* val = Py_GETENV(name);
  if (val == NULL || val[0] == '\0' || !strncmp(val, "0", 1)) {
    return 0;
  }
  return 1;
}

int _is_flag_set(const char* xoption, const char* envname) {
  if (PyJIT_IsXOptionSet(xoption) || _is_env_truthy(envname)) {
    return 1;
  }
  return 0;
}

// If the given X option is set and is a string, return it. If not, check the
// given environment variable for a nonempty value and return it if
// found. Otherwise, return nullptr.
const char* flag_string(const char* xoption, const char* envname) {
  PyObject* pyobj = nullptr;
  if (PyJIT_GetXOption(xoption, &pyobj) == 0 && pyobj != nullptr &&
      PyUnicode_Check(pyobj)) {
    return PyUnicode_AsUTF8(pyobj);
  }

  auto envval = Py_GETENV(envname);
  if (envval != nullptr && envval[0] != '\0') {
    return envval;
  }

  return nullptr;
}

long flag_long(const char* xoption, const char* envname, long _default) {
  PyObject* pyobj = nullptr;
  if (PyJIT_GetXOption(xoption, &pyobj) == 0 && pyobj != nullptr &&
      PyUnicode_Check(pyobj)) {
    auto val = Ref<PyObject>::steal(PyLong_FromUnicodeObject(pyobj, 10));
    if (val != nullptr) {
      return PyLong_AsLong(val);
    }
    JIT_LOG("Invalid value for %s: %s", xoption, PyUnicode_AsUTF8(pyobj));
  }

  const char* envval = Py_GETENV(envname);
  if (envval != nullptr && envval[0] != '\0') {
    try {
      return std::stol(envval);
    } catch (std::exception const&) {
      JIT_LOG("Invalid value for %s: %s", envname, envval);
    }
  }

  return _default;
}

int _PyJIT_Initialize() {
  if (jit_config.init_state == JIT_INITIALIZED) {
    return 0;
  }

  int use_jit = 0;

  if (_is_flag_set("jit", "PYTHONJIT")) {
    use_jit = 1;
  }

  // Redirect logging to a file if configured.
  const char* log_filename = flag_string("jit-log-file", "PYTHONJITLOGFILE");
  if (log_filename != nullptr) {
    const char* kPidMarker = "{pid}";
    std::string pid_filename = log_filename;
    auto marker_pos = pid_filename.find(kPidMarker);
    if (marker_pos != std::string::npos) {
      pid_filename.replace(
          marker_pos, std::strlen(kPidMarker), fmt::format("{}", getpid()));
    }
    FILE* file = fopen(pid_filename.c_str(), "w");
    if (file == NULL) {
      JIT_LOG(
          "Couldn't open log file %s (%s), logging to stderr",
          pid_filename,
          strerror(errno));
    } else {
      g_log_file = file;
    }
  }

  if (_is_flag_set("jit-debug", "PYTHONJITDEBUG")) {
    JIT_DLOG("Enabling JIT debug and extra logging.");
    g_debug = 1;
    g_debug_verbose = 1;
  }
  if (_is_flag_set("jit-debug-refcount", "PYTHONJITDEBUGREFCOUNT")) {
    JIT_DLOG("Enabling JIT refcount insertion debug mode.");
    g_debug_refcount = 1;
  }
  if (_is_flag_set("jit-dump-hir", "PYTHONJITDUMPHIR")) {
    JIT_DLOG("Enabling JIT dump-hir mode.");
    g_dump_hir = 1;
  }
  if (_is_flag_set("jit-dump-hir-passes", "PYTHONJITDUMPHIRPASSES")) {
    JIT_DLOG("Enabling JIT dump-hir-passes mode.");
    g_dump_hir_passes = 1;
  }
  if (_is_flag_set("jit-dump-final-hir", "PYTHONJITDUMPFINALHIR")) {
    JIT_DLOG("Enabling JIT dump-final-hir mode.");
    g_dump_final_hir = 1;
  }
  if (_is_flag_set("jit-dump-lir", "PYTHONJITDUMPLIR")) {
    JIT_DLOG("Enable JIT dump-lir mode with origin data.");
    g_dump_lir = 1;
  }
  if (_is_flag_set("jit-dump-lir-no-origin", "PYTHONJITDUMPLIRNOORIGIN")) {
    JIT_DLOG("Enable JIT dump-lir mode without origin data.");
    g_dump_lir = 1;
    g_dump_lir_no_origin = 1;
  }
  if (_is_flag_set("jit-disas-funcs", "PYTHONJITDISASFUNCS")) {
    JIT_DLOG("Enabling JIT disas-funcs mode.");
    g_disas_funcs = 1;
  }
  if (_is_flag_set("jit-gdb-support", "PYTHONJITGDBSUPPORT")) {
    JIT_DLOG("Enable GDB support and JIT debug mode.");
    g_debug = 1;
    g_gdb_support = 1;
  }
  if (_is_flag_set("jit-gdb-stubs-support", "PYTHONJITGDBSUPPORT")) {
    JIT_DLOG("Enable GDB support for stubs.");
    g_gdb_stubs_support = 1;
  }
  if (_is_flag_set("jit-gdb-write-elf", "PYTHONJITGDBWRITEELF")) {
    JIT_DLOG("Enable GDB support with ELF output, and JIT debug.");
    g_debug = 1;
    g_gdb_support = 1;
    g_gdb_write_elf_objects = 1;
  }

  if (_is_flag_set(
          "jit-enable-jit-list-wildcards", "PYTHONJITENABLEJITLISTWILDCARDS")) {
    JIT_LOG("Enabling wildcards in JIT list");
    jit_config.allow_jit_list_wildcards = 1;
  }
  if (_is_flag_set("jit-all-static-functions", "PYTHONJITALLSTATICFUNCTIONS")) {
    JIT_DLOG("JIT-compiling all static functions");
    jit_config.compile_all_static_functions = 1;
  }

  std::unique_ptr<JITList> jit_list;
  const char* jl_fn = flag_string("jit-list-file", "PYTHONJITLISTFILE");
  if (jl_fn != NULL) {
    use_jit = 1;

    if (jit_config.allow_jit_list_wildcards) {
      jit_list = jit::WildcardJITList::create();
    } else {
      jit_list = jit::JITList::create();
    }
    if (jit_list == nullptr) {
      JIT_LOG("Failed to allocate JIT list");
      return -1;
    }
    if (!jit_list->parseFile(jl_fn)) {
      JIT_LOG("Could not parse jit-list, disabling JIT.");
      return 0;
    }
  }

  if (use_jit) {
    JIT_DLOG("Enabling JIT.");
  } else {
    return 0;
  }

  if (_PyJITContext_Init() == -1) {
    JIT_LOG("failed initializing jit context");
    return -1;
  }

  jit_ctx = _PyJITContext_New(std::make_unique<jit::Compiler>());
  if (jit_ctx == NULL) {
    JIT_LOG("failed creating global jit context");
    return -1;
  }

  PyObject* mod = PyModule_Create(&jit_module);
  if (mod == NULL) {
    return -1;
  }

  PyObject* modname = PyUnicode_InternFromString("cinderjit");
  if (modname == NULL) {
    return -1;
  }

  PyObject* modules = PyImport_GetModuleDict();
  int st = _PyImport_FixupExtensionObject(mod, modname, modname, modules);
  Py_DECREF(modname);
  if (st == -1) {
    return -1;
  }

  jit_config.init_state = JIT_INITIALIZED;
  jit_config.is_enabled = 1;
  g_jit_list = jit_list.release();
  if (_is_flag_set("jit-tiny-frame", "PYTHONJITTINYFRAME")) {
    jit_config.frame_mode = TINY_FRAME;
  }
  if (_is_flag_set("jit-no-frame", "PYTHONJITNOFRAME")) {
    JIT_CHECK(
        jit_config.frame_mode == PY_FRAME,
        "-X jit-tiny-frame and -X jit-no-frame are mutually exclusive.");
    jit_config.frame_mode = NO_FRAME;
  }
  jit_config.are_type_slots_enabled = !PyJIT_IsXOptionSet("jit-no-type-slots");
  jit_config.batch_compile_workers =
      flag_long("jit-batch-compile-workers", "PYTHONJITBATCHCOMPILEWORKERS", 0);
  if (_is_flag_set(
          "jit-test-multithreaded-compile",
          "PYTHONJITTESTMULTITHREADEDCOMPILE")) {
    jit_config.test_multithreaded_compile = 1;
  }

  total_compliation_time = 0.0;

  return 0;
}

int _PyJIT_IsEnabled() {
  return (jit_config.init_state == JIT_INITIALIZED) && jit_config.is_enabled;
}

void _PyJIT_AfterFork_Child() {
  perf::afterForkChild();
}

int _PyJIT_AreTypeSlotsEnabled() {
  return (jit_config.init_state == JIT_INITIALIZED) &&
      jit_config.are_type_slots_enabled;
}

int _PyJIT_Enable() {
  if (jit_config.init_state != JIT_INITIALIZED) {
    return 0;
  }
  jit_config.is_enabled = 1;
  return 0;
}

int _PyJIT_EnableTypeSlots() {
  if (!_PyJIT_IsEnabled()) {
    return 0;
  }
  jit_config.are_type_slots_enabled = 1;
  return 1;
}

void _PyJIT_Disable() {
  jit_config.is_enabled = 0;
  jit_config.are_type_slots_enabled = 0;
}

_PyJIT_Result _PyJIT_SpecializeType(
    PyTypeObject* type,
    _PyJIT_TypeSlots* slots) {
  return _PyJITContext_SpecializeType(jit_ctx, type, slots);
}

_PyJIT_Result _PyJIT_CompileFunction(PyFunctionObject* func) {
  // Serialize here as we might have been called re-entrantly.
  ThreadedCompileSerialize guard;

  if (jit_ctx == nullptr) {
    return PYJIT_NOT_INITIALIZED;
  }

  // The list of conditions here should be matched in compile_worker_thread()
  if (_PyJIT_IsCompiled(reinterpret_cast<PyObject*>(func))) {
    return PYJIT_RESULT_OK;
  }
  if (!_PyJIT_OnJitList(func)) {
    return PYJIT_RESULT_CANNOT_SPECIALIZE;
  }

  CompilationTimer timer(func);
  const int kMaxCompileDepth = 10;
  static std::vector<PyObject*> active_compiles;
  // Don't attempt the compilation if there are already too many active
  // compilations or this function's code is one of them.
  if (active_compiles.size() == kMaxCompileDepth ||
      std::find(
          active_compiles.begin(), active_compiles.end(), func->func_code) !=
          active_compiles.end()) {
    return PYJIT_RESULT_UNKNOWN_ERROR;
  }

  jit_reg_functions.erase(func);
  active_compiles.push_back(func->func_code);
  _PyJIT_Result result = _PyJITContext_CompileFunction(jit_ctx, func);
  active_compiles.pop_back();
  return result;
}

int _PyJIT_RegisterFunction(PyFunctionObject* func) {
  if (_PyJIT_IsEnabled() && _PyJIT_OnJitList(func)) {
    if (jit_config.test_multithreaded_compile) {
      test_multithreaded_funcs.emplace_back(func);
    }
    jit_reg_functions.insert(func);
    return 1;
  }
  return 0;
}

void _PyJIT_UnregisterFunction(PyFunctionObject* func) {
  if (_PyJIT_IsEnabled()) {
    jit_reg_functions.erase(func);
  }
}

int _PyJIT_Finalize() {
  // Always release references from Runtime objects: C++ clients may have
  // invoked the JIT directly without initializing a full _PyJITContext.
  jit::codegen::NativeGenerator::runtime()->releaseReferences();

  if (jit_config.init_state != JIT_INITIALIZED) {
    return 0;
  }

  delete g_jit_list;
  g_jit_list = nullptr;

  jit_config.init_state = JIT_FINALIZED;

  JIT_CHECK(jit_ctx != NULL, "jit_ctx not initialized");
  Py_CLEAR(jit_ctx);

  return 0;
}

int _PyJIT_TinyFrame() {
  return jit_config.frame_mode == TINY_FRAME;
}

int _PyJIT_NoFrame() {
  return jit_config.frame_mode == NO_FRAME;
}

PyObject* _PyJIT_GenSend(
    PyGenObject* gen,
    PyObject* arg,
    int exc,
    PyFrameObject* f,
    PyThreadState* tstate,
    int finish_yield_from) {
  auto gen_footer = reinterpret_cast<GenDataFooter*>(gen->gi_jit_data);

  // state should be valid and the generator should not be completed
  JIT_DCHECK(
      gen_footer->state == _PyJitGenState_JustStarted ||
          gen_footer->state == _PyJitGenState_Running,
      "Invalid JIT generator state");

  gen_footer->state = _PyJitGenState_Running;

  // JIT generators use NULL arg to indicate an exception
  if (exc) {
    JIT_DCHECK(
        arg == Py_None, "Arg should be None when injecting an exception");
    Py_DECREF(arg);
    arg = NULL;
  } else {
    if (arg == NULL) {
      arg = Py_None;
      Py_INCREF(arg);
    }
  }

  if (f) {
    // Setup tstate/frame as would be done in PyEval_EvalFrameEx() or
    // prologue of a JITed function.
    tstate->frame = f;
    f->f_executing = 1;
    // This compensates for the decref which occurs in JITRT_UnlinkFrame().
    Py_INCREF(f);
    // This satisfies code which uses f_lasti == -1 or < 0 to check if a
    // generator is not yet started, but still provides a garbage value in case
    // anything tries to actually use f_lasti.
    f->f_lasti = std::numeric_limits<int>::max();
  }

  // Enter generated code.
  JIT_DCHECK(
      gen_footer->yieldPoint != nullptr,
      "Attempting to resume a generator with no yield point");
  PyObject* result =
      gen_footer->resumeEntry((PyObject*)gen, arg, tstate, finish_yield_from);

  if (!result) {
    gen_footer->state = _PyJitGenState_Completed;
  }

  return result;
}

int _PyJIT_GenVisitRefs(PyGenObject* gen, visitproc visit, void* arg) {
  auto gen_footer = reinterpret_cast<GenDataFooter*>(gen->gi_jit_data);
  JIT_DCHECK(gen_footer, "Generator missing JIT data");
  if (gen_footer->state != _PyJitGenState_Completed && gen_footer->yieldPoint) {
    return reinterpret_cast<GenYieldPoint*>(gen_footer->yieldPoint)
        ->visitRefs(gen, visit, arg);
  }
  return 0;
}

void _PyJIT_GenDealloc(PyGenObject* gen) {
  auto gen_footer = reinterpret_cast<GenDataFooter*>(gen->gi_jit_data);
  JIT_DCHECK(gen_footer, "Generator missing JIT data");
  if (gen_footer->state != _PyJitGenState_Completed && gen_footer->yieldPoint) {
    reinterpret_cast<GenYieldPoint*>(gen_footer->yieldPoint)->releaseRefs(gen);
  }
  JITRT_GenJitDataFree(gen);
}

PyObject* _PyJIT_GenYieldFromValue(PyGenObject* gen) {
  auto gen_footer = reinterpret_cast<GenDataFooter*>(gen->gi_jit_data);
  JIT_DCHECK(gen_footer, "Generator missing JIT data");
  PyObject* yf = NULL;
  if (gen_footer->state != _PyJitGenState_Completed && gen_footer->yieldPoint) {
    yf = gen_footer->yieldPoint->yieldFromValue(gen_footer);
    Py_XINCREF(yf);
  }
  return yf;
}
