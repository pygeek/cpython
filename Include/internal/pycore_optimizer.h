#ifndef Py_INTERNAL_OPTIMIZER_H
#define Py_INTERNAL_OPTIMIZER_H
#ifdef __cplusplus
extern "C" {
#endif

#ifndef Py_BUILD_CORE
#  error "this header requires Py_BUILD_CORE define"
#endif

#include "pycore_uop_ids.h"
#include <stdbool.h>


typedef struct _PyExecutorLinkListNode {
    struct _PyExecutorObject *next;
    struct _PyExecutorObject *previous;
} _PyExecutorLinkListNode;


/* Bloom filter with m = 256
 * https://en.wikipedia.org/wiki/Bloom_filter */
#define _Py_BLOOM_FILTER_WORDS 8

typedef struct {
    uint32_t bits[_Py_BLOOM_FILTER_WORDS];
} _PyBloomFilter;

typedef struct {
    uint8_t opcode;
    uint8_t oparg;
    uint8_t valid:1;
    uint8_t linked:1;
    uint8_t chain_depth:6;  // Must be big enough for MAX_CHAIN_DEPTH - 1.
    bool warm;
    int index;           // Index of ENTER_EXECUTOR (if code isn't NULL, below).
    _PyBloomFilter bloom;
    _PyExecutorLinkListNode links;
    PyCodeObject *code;  // Weak (NULL if no corresponding ENTER_EXECUTOR).
} _PyVMData;

/* Depending on the format,
 * the 32 bits between the oparg and operand are:
 * UOP_FORMAT_TARGET:
 *    uint32_t target;
 * UOP_FORMAT_JUMP
 *    uint16_t jump_target;
 *    uint16_t error_target;
 */
typedef struct {
    uint16_t opcode:15;
    uint16_t format:1;
    uint16_t oparg;
    union {
        uint32_t target;
        struct {
            uint16_t jump_target;
            uint16_t error_target;
        };
    };
    uint64_t operand0;  // A cache entry
    uint64_t operand1;
#ifdef Py_STATS
    uint64_t execution_count;
#endif
} _PyUOpInstruction;

typedef struct {
    uint32_t target;
    _Py_BackoffCounter temperature;
    const struct _PyExecutorObject *executor;
} _PyExitData;

typedef struct _PyExecutorObject {
    PyObject_VAR_HEAD
    const _PyUOpInstruction *trace;
    _PyVMData vm_data; /* Used by the VM, but opaque to the optimizer */
    uint32_t exit_count;
    uint32_t code_size;
    size_t jit_size;
    void *jit_code;
    void *jit_side_entry;
    _PyExitData exits[1];
} _PyExecutorObject;

typedef struct _PyOptimizerObject _PyOptimizerObject;

/* Should return > 0 if a new executor is created. O if no executor is produced and < 0 if an error occurred. */
typedef int (*_Py_optimize_func)(
    _PyOptimizerObject* self, struct _PyInterpreterFrame *frame,
    _Py_CODEUNIT *instr, _PyExecutorObject **exec_ptr,
    int curr_stackentries, bool progress_needed);

struct _PyOptimizerObject {
    PyObject_HEAD
    _Py_optimize_func optimize;
    /* Data needed by the optimizer goes here, but is opaque to the VM */
};

/** Test support **/
_PyOptimizerObject *_Py_SetOptimizer(PyInterpreterState *interp, _PyOptimizerObject* optimizer);


// Export for '_opcode' shared extension (JIT compiler).
PyAPI_FUNC(_PyExecutorObject*) _Py_GetExecutor(PyCodeObject *code, int offset);

void _Py_ExecutorInit(_PyExecutorObject *, const _PyBloomFilter *);
void _Py_ExecutorDetach(_PyExecutorObject *);
void _Py_BloomFilter_Init(_PyBloomFilter *);
void _Py_BloomFilter_Add(_PyBloomFilter *bloom, void *obj);
PyAPI_FUNC(void) _Py_Executor_DependsOn(_PyExecutorObject *executor, void *obj);

// For testing
// Export for '_testinternalcapi' shared extension.
PyAPI_FUNC(_PyOptimizerObject *) _Py_GetOptimizer(void);
PyAPI_FUNC(int) _Py_SetTier2Optimizer(_PyOptimizerObject* optimizer);
PyAPI_FUNC(PyObject *) _PyOptimizer_NewUOpOptimizer(void);

#define _Py_MAX_ALLOWED_BUILTINS_MODIFICATIONS 3
#define _Py_MAX_ALLOWED_GLOBALS_MODIFICATIONS 6

#ifdef _Py_TIER2
PyAPI_FUNC(void) _Py_Executors_InvalidateDependency(PyInterpreterState *interp, void *obj, int is_invalidation);
PyAPI_FUNC(void) _Py_Executors_InvalidateAll(PyInterpreterState *interp, int is_invalidation);
PyAPI_FUNC(void) _Py_Executors_InvalidateCold(PyInterpreterState *interp);

#else
#  define _Py_Executors_InvalidateDependency(A, B, C) ((void)0)
#  define _Py_Executors_InvalidateAll(A, B) ((void)0)
#  define _Py_Executors_InvalidateCold(A) ((void)0)

#endif

// Used as the threshold to trigger executor invalidation when
// trace_run_counter is greater than this value.
#define JIT_CLEANUP_THRESHOLD 100000

// This is the length of the trace we project initially.
#define UOP_MAX_TRACE_LENGTH 800

#define TRACE_STACK_SIZE 5

int _Py_uop_analyze_and_optimize(struct _PyInterpreterFrame *frame,
    _PyUOpInstruction *trace, int trace_len, int curr_stackentries,
    _PyBloomFilter *dependencies);

extern PyTypeObject _PyDefaultOptimizer_Type;
extern PyTypeObject _PyUOpExecutor_Type;
extern PyTypeObject _PyUOpOptimizer_Type;

/* Symbols */
/* See explanation in optimizer_symbols.c */

struct _Py_UopsSymbol {
    int flags;  // 0 bits: Top; 2 or more bits: Bottom
    PyTypeObject *typ;  // Borrowed reference
    PyObject *const_val;  // Owned reference (!)
    unsigned int type_version; // currently stores type version
};

#define UOP_FORMAT_TARGET 0
#define UOP_FORMAT_JUMP 1

static inline uint32_t uop_get_target(const _PyUOpInstruction *inst)
{
    assert(inst->format == UOP_FORMAT_TARGET);
    return inst->target;
}

static inline uint16_t uop_get_jump_target(const _PyUOpInstruction *inst)
{
    assert(inst->format == UOP_FORMAT_JUMP);
    return inst->jump_target;
}

static inline uint16_t uop_get_error_target(const _PyUOpInstruction *inst)
{
    assert(inst->format != UOP_FORMAT_TARGET);
    return inst->error_target;
}

// Holds locals, stack, locals, stack ... co_consts (in that order)
#define MAX_ABSTRACT_INTERP_SIZE 4096

#define TY_ARENA_SIZE (UOP_MAX_TRACE_LENGTH * 5)

// Need extras for root frame and for overflow frame (see TRACE_STACK_PUSH())
#define MAX_ABSTRACT_FRAME_DEPTH (TRACE_STACK_SIZE + 2)

// The maximum number of side exits that we can take before requiring forward
// progress (and inserting a new ENTER_EXECUTOR instruction). In practice, this
// is the "maximum amount of polymorphism" that an isolated trace tree can
// handle before rejoining the rest of the program.
#define MAX_CHAIN_DEPTH 4

typedef struct _Py_UopsSymbol _Py_UopsSymbol;

struct _Py_UOpsAbstractFrame {
    // Max stacklen
    int stack_len;
    int locals_len;

    _Py_UopsSymbol **stack_pointer;
    _Py_UopsSymbol **stack;
    _Py_UopsSymbol **locals;
};

typedef struct _Py_UOpsAbstractFrame _Py_UOpsAbstractFrame;

typedef struct ty_arena {
    int ty_curr_number;
    int ty_max_number;
    _Py_UopsSymbol arena[TY_ARENA_SIZE];
} ty_arena;

struct _Py_UOpsContext {
    char done;
    char out_of_space;
    bool contradiction;
    // The current "executing" frame.
    _Py_UOpsAbstractFrame *frame;
    _Py_UOpsAbstractFrame frames[MAX_ABSTRACT_FRAME_DEPTH];
    int curr_frame_depth;

    // Arena for the symbolic types.
    ty_arena t_arena;

    _Py_UopsSymbol **n_consumed;
    _Py_UopsSymbol **limit;
    _Py_UopsSymbol *locals_and_stack[MAX_ABSTRACT_INTERP_SIZE];
};

typedef struct _Py_UOpsContext _Py_UOpsContext;

extern bool _Py_uop_sym_is_null(_Py_UopsSymbol *sym);
extern bool _Py_uop_sym_is_not_null(_Py_UopsSymbol *sym);
extern bool _Py_uop_sym_is_const(_Py_UopsSymbol *sym);
extern PyObject *_Py_uop_sym_get_const(_Py_UopsSymbol *sym);
extern _Py_UopsSymbol *_Py_uop_sym_new_unknown(_Py_UOpsContext *ctx);
extern _Py_UopsSymbol *_Py_uop_sym_new_not_null(_Py_UOpsContext *ctx);
extern _Py_UopsSymbol *_Py_uop_sym_new_type(
    _Py_UOpsContext *ctx, PyTypeObject *typ);
extern _Py_UopsSymbol *_Py_uop_sym_new_const(_Py_UOpsContext *ctx, PyObject *const_val);
extern _Py_UopsSymbol *_Py_uop_sym_new_null(_Py_UOpsContext *ctx);
extern bool _Py_uop_sym_has_type(_Py_UopsSymbol *sym);
extern bool _Py_uop_sym_matches_type(_Py_UopsSymbol *sym, PyTypeObject *typ);
extern bool _Py_uop_sym_matches_type_version(_Py_UopsSymbol *sym, unsigned int version);
extern void _Py_uop_sym_set_null(_Py_UOpsContext *ctx, _Py_UopsSymbol *sym);
extern void _Py_uop_sym_set_non_null(_Py_UOpsContext *ctx, _Py_UopsSymbol *sym);
extern void _Py_uop_sym_set_type(_Py_UOpsContext *ctx, _Py_UopsSymbol *sym, PyTypeObject *typ);
extern bool _Py_uop_sym_set_type_version(_Py_UOpsContext *ctx, _Py_UopsSymbol *sym, unsigned int version);
extern void _Py_uop_sym_set_const(_Py_UOpsContext *ctx, _Py_UopsSymbol *sym, PyObject *const_val);
extern bool _Py_uop_sym_is_bottom(_Py_UopsSymbol *sym);
extern int _Py_uop_sym_truthiness(_Py_UopsSymbol *sym);
extern PyTypeObject *_Py_uop_sym_get_type(_Py_UopsSymbol *sym);


extern void _Py_uop_abstractcontext_init(_Py_UOpsContext *ctx);
extern void _Py_uop_abstractcontext_fini(_Py_UOpsContext *ctx);

extern _Py_UOpsAbstractFrame *_Py_uop_frame_new(
    _Py_UOpsContext *ctx,
    PyCodeObject *co,
    int curr_stackentries,
    _Py_UopsSymbol **args,
    int arg_len);
extern int _Py_uop_frame_pop(_Py_UOpsContext *ctx);

PyAPI_FUNC(PyObject *) _Py_uop_symbols_test(PyObject *self, PyObject *ignored);

PyAPI_FUNC(int) _PyOptimizer_Optimize(struct _PyInterpreterFrame *frame, _Py_CODEUNIT *start, _PyStackRef *stack_pointer, _PyExecutorObject **exec_ptr, int chain_depth);

static inline int is_terminator(const _PyUOpInstruction *uop)
{
    int opcode = uop->opcode;
    return (
        opcode == _EXIT_TRACE ||
        opcode == _JUMP_TO_TOP ||
        opcode == _DYNAMIC_EXIT
    );
}

PyAPI_FUNC(int) _PyDumpExecutors(FILE *out);

#ifdef __cplusplus
}
#endif
#endif /* !Py_INTERNAL_OPTIMIZER_H */
