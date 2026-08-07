#ifndef BEE_BUFFER_H
#define BEE_BUFFER_H
/*
 * BeeBuffer -- how bulk data crosses into a native library.
 *
 * A plain C struct on purpose: a shim that binds OpenCV, ONNX Runtime, TensorRT
 * or any other library can take BeeBuffer parameters without including a single
 * BeeLang header, and `beegen` recognises the type and passes a BeeLang buffer
 * straight through. `data` points at the buffer's own memory, so nothing is
 * copied -- a 4K image crosses as one pointer.
 *
 * The pointer is valid only for the duration of the call. A library that keeps
 * the memory (an async inference queue, a GPU upload that outlives the call)
 * must copy it.
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BEE_DTYPE_F32 = 0,
    BEE_DTYPE_F64 = 1,
    BEE_DTYPE_I8  = 2,
    BEE_DTYPE_U8  = 3,
    BEE_DTYPE_I16 = 4,
    BEE_DTYPE_U16 = 5,
    BEE_DTYPE_I32 = 6,
    BEE_DTYPE_I64 = 7
};

#define BEE_BUFFER_MAX_DIMS 8

typedef struct BeeBuffer {
    void*     data;                        /* first element, or NULL if empty   */
    long long bytes;                       /* total length in bytes             */
    int       dtype;                       /* one of BEE_DTYPE_*                */
    int       ndim;                        /* 0 for a scalar                    */
    long long shape[BEE_BUFFER_MAX_DIMS];  /* row-major, ndim entries used      */
} BeeBuffer;

/* Element count, for shims that would rather not multiply the shape out. */
static inline long long bee_buffer_count(const BeeBuffer* b) {
    long long n = 1;
    int i;
    if (b->ndim == 0) return b->bytes ? 1 : 0;
    for (i = 0; i < b->ndim; ++i) n *= b->shape[i];
    return n;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif
#endif  /* BEE_BUFFER_H */
