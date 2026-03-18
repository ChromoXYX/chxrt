#pragma once

/**
 * @brief chxrt: A static radix tree implementation for URI Path matching.
 *
 */

#include <stdint.h>
#include <stddef.h>

struct chxrt_tree_st;
/**
 * @brief Opaque radix tree handle.
 */
typedef struct chxrt_tree_st chxrt_tree;

/**
 * @brief Parameter kinds supported by route placeholders.
 *
 * Placeholders in route patterns:
 * - <int>
 * - <uint>
 * - <str>
 */
enum chxrt_param_type_enum : uint8_t {
    /** Not set / invalid. */
    CHXRT_PARAM_VOID = 0,
    /** Unsigned integer parsed from a path segment. */
    CHXRT_PARAM_UINT = 1,
    /** Signed integer parsed from a path segment. */
    CHXRT_PARAM_INT = 2,
    /** Raw string slice of a path segment. */
    CHXRT_PARAM_STR = 3,
};
typedef enum chxrt_param_type_enum chxrt_param_type;

/**
 * @brief Decoded placeholder value returned by chxrt_lookup().
 *
 * For CHXRT_PARAM_STR, the string is a non-owning slice [str_begin, str_end)
 * into the input key passed to chxrt_lookup().
 */
struct chxrt_param_st {
    /** Actual type stored in the union. */
    enum chxrt_param_type_enum type;
    union {
        /** Value for CHXRT_PARAM_UINT. */
        uint64_t uint_value;
        /** Value for CHXRT_PARAM_INT. */
        int64_t int_value;
        struct {
            /** Begin pointer for CHXRT_PARAM_STR (inclusive). */
            const char* str_begin;
            /** End pointer for CHXRT_PARAM_STR (exclusive). */
            const char* str_end;
        };
    };
};
typedef struct chxrt_param_st chxrt_param;

/**
 * @brief Create a new route tree.
 *
 * @return Non-NULL on success, NULL on allocation failure.
 */
[[nodiscard]] struct chxrt_tree_st* chxrt_new();

/**
 * @brief Insert one route pattern and its associated user value.
 *
 * Pattern supports static text and placeholders: <int>, <uint>, <str>.
 *
 * @param tree Tree created by chxrt_new(). Must not be compiled yet.
 * @param key Route pattern buffer (not required to be null-terminated).
 * @param key_len Pattern length in bytes.
 * @param value User payload pointer returned by chxrt_lookup() on match.
 * @return 0 on success, -1 on invalid pattern/conflict/out-of-memory/or if
 *         tree is already compiled.
 */
int chxrt_insert(struct chxrt_tree_st* tree, const char* key, size_t key_len,
                 void* value);

int chxrt_find(const struct chxrt_tree_st* tree, const char* key,
               size_t key_len, void*** slot);

int chxrt_acquire(const struct chxrt_tree_st* tree, const char* key,
                  size_t key_len, void*** slot);

/**
 * @brief Compile inserted patterns into a compact lookup structure.
 *
 * After successful compilation, insertion is no longer allowed.
 *
 * @param tree Tree created by chxrt_new() and filled by chxrt_insert().
 * @param heap_alloc_bytes Optional output for total heap bytes used during
 *        compile (compiled nodes + string pool + temporary queue).
 * @return 0 on success, -1 on failure.
 */
int chxrt_compile(struct chxrt_tree_st* tree, size_t* heap_alloc_bytes);

/**
 * @brief Match a concrete path and decode placeholder values.
 *
 * @param tree Compiled tree.
 * @param key Input path buffer (not required to be null-terminated).
 * @param key_len Input path length in bytes.
 * @param out Output user payload for the matched route.
 * @param param_list Output buffer for decoded placeholders.
 * @param param_list_n Number of entries available in param_list.
 * @return 0 on success.
 * @return -1 when not compiled, no route matches, or internal mismatch.
 * @return -2 when param_list capacity is insufficient.
 * @return -3 when parameter parsing fails (<int>/<uint> conversion error).
 */
[[nodiscard]] int chxrt_lookup(const struct chxrt_tree_st* tree,
                               const char* key, size_t key_len, void** out,
                               struct chxrt_param_st* param_list,
                               size_t param_list_n);

/**
 * @brief Destroy a tree and release all owned memory.
 *
 * Safe to call with NULL.
 *
 * @param tree Tree handle returned by chxrt_new().
 */
void chxrt_delete(struct chxrt_tree_st* tree);

void chxrt_visit(struct chxrt_tree_st* tree,
                 void (*visitor)(void* node, void* ud), void* ud);
