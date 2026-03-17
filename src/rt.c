#include "chx/rt.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline uint16_t load_16bits(const uint8_t* p) {
    uint16_t r;
    memcpy(&r, p, 2);
    return r;
}

static inline uint32_t load_32bits(const uint8_t* p) {
    uint32_t r;
    memcpy(&r, p, 4);
    return r;
}

static inline int faster_memcmp(const void* p1, const void* p2, size_t n) {
    const uint8_t* s1 = (const uint8_t*)p1;
    const uint8_t* s2 = (const uint8_t*)p2;

    if (n < 8) {
        switch (n) {
        case 0:
            return 0;
        case 1:
            return *s1 != *s2;
        case 2:
            return load_16bits(s1) != load_16bits(s2);
        case 3:
            return *s1 != *s2 || (load_16bits(s1 + 1) != load_16bits(s2 + 1));
        default: {
            return (load_32bits(s1) != load_32bits(s2)) |
                   (load_32bits(s1 + n - 4) != load_32bits(s2 + n - 4));
        }
        }
    }

    uint64_t v1, v2;
    memcpy(&v1, s1, 8);
    memcpy(&v2, s2, 8);
    if (v1 != v2)
        return 1;

    memcpy(&v1, s1 + n - 8, 8);
    memcpy(&v2, s2 + n - 8, 8);
    if (v1 != v2)
        return 1;

    if (n <= 16)
        return 0;

    s1 += 8;
    s2 += 8;
    n -= 16;

    while (n >= 8) {
        memcpy(&v1, s1, 8);
        memcpy(&v2, s2, 8);
        if (v1 != v2)
            return 1;
        s1 += 8;
        s2 += 8;
        n -= 8;
    }

    return 0;
}

typedef struct inode_s {
    bool is_param;
    bool is_leaf;
    chxrt_param_type param_type;

    char* prefix;
    size_t prefix_len;

    void* user_data;

    struct inode_s** children;
    size_t n_children;
    size_t cap_children;
} inode_t;

typedef struct {
    void* user_data;
    uint32_t prefix_offset;
    uint32_t prefix_len;
    uint32_t first_child;
    uint16_t n_children;
    bool is_param;
    bool is_leaf;
    chxrt_param_type param_type;

} cnode_t;

struct chxrt_tree_st {
    inode_t* root;

    cnode_t* compiled;
    size_t n_compiled;
    char* string_pool;

    bool is_compiled;
};

static inode_t* inode_new(void) { return calloc(1, sizeof(inode_t)); }

static inode_t* inode_new_static(const char* prefix, size_t len) {
    inode_t* n = inode_new();
    if (!n)
        return NULL;
    n->prefix = malloc(len);
    if (!n->prefix) {
        free(n);
        return NULL;
    }
    memcpy(n->prefix, prefix, len);
    n->prefix_len = len;
    return n;
}

static inode_t* inode_new_param(chxrt_param_type type) {
    inode_t* n = inode_new();
    if (!n)
        return NULL;
    n->is_param = true;
    n->param_type = type;
    return n;
}

static bool inode_add_child(inode_t* parent, inode_t* child) {
    if (parent->n_children >= parent->cap_children) {
        size_t new_cap = parent->cap_children ? parent->cap_children * 2 : 4;
        inode_t** new_children =
            realloc(parent->children, new_cap * sizeof(inode_t*));
        if (!new_children)
            return false;
        parent->children = new_children;
        parent->cap_children = new_cap;
    }
    parent->children[parent->n_children++] = child;
    return true;
}

static void inode_free(inode_t* node) {
    if (!node)
        return;
    for (size_t i = 0; i < node->n_children; i++)
        inode_free(node->children[i]);
    free(node->children);
    free(node->prefix);
    free(node);
}

typedef struct {
    bool is_param;
    chxrt_param_type param_type;
    const char* str;
    size_t len;
} segment_t;

#define MAX_SEGMENTS 128

static int parse_pattern(const char* pat, size_t pat_len, segment_t* segs,
                         size_t max_segs) {
    size_t n = 0;
    size_t pos = 0;
    size_t stat_start = 0;

    while (pos < pat_len) {
        if (pat[pos] != '<') {
            pos++;
            continue;
        }

        if (pos > stat_start) {
            if (n >= max_segs)
                return -1;
            segs[n++] = (segment_t){
                .is_param = false,
                .str = pat + stat_start,
                .len = pos - stat_start,
            };
        }

        size_t close = pos + 1;
        while (close < pat_len && pat[close] != '>')
            close++;
        if (close >= pat_len)
            return -1;

        const char* tag = pat + pos + 1;
        size_t tag_len = close - pos - 1;

        if (n >= max_segs)
            return -1;

        chxrt_param_type pt;
        if (tag_len == 3 && memcmp(tag, "int", 3) == 0)
            pt = CHXRT_PARAM_INT;
        else if (tag_len == 4 && memcmp(tag, "uint", 4) == 0)
            pt = CHXRT_PARAM_UINT;
        else if (tag_len == 3 && memcmp(tag, "str", 3) == 0)
            pt = CHXRT_PARAM_STR;
        else
            return -1;

        segs[n++] = (segment_t){
            .is_param = true,
            .param_type = pt,
        };

        pos = close + 1;
        stat_start = pos;
    }

    if (pos > stat_start) {
        if (n >= max_segs)
            return -1;
        segs[n++] = (segment_t){
            .is_param = false,
            .str = pat + stat_start,
            .len = pos - stat_start,
        };
    }

    return (int)n;
}

static size_t prefix_common_len(const char* a, size_t alen, const char* b,
                                size_t blen) {
    size_t lim = alen < blen ? alen : blen;
    for (size_t i = 0; i < lim; i++) {
        if (a[i] != b[i])
            return i;
    }
    return lim;
}

static inode_t* insert_static(inode_t* node, const char* str, size_t len) {
    if (len == 0)
        return node;

    for (size_t i = 0; i < node->n_children; i++) {
        if (node->children[i]->is_param)
            return NULL;
    }

    for (size_t i = 0; i < node->n_children; i++) {
        inode_t* child = node->children[i];
        size_t cpl =
            prefix_common_len(child->prefix, child->prefix_len, str, len);
        if (cpl == 0)
            continue;

        if (cpl == child->prefix_len && cpl == len) {
            return child;
        }

        if (cpl == child->prefix_len) {
            return insert_static(child, str + cpl, len - cpl);
        }

        if (cpl == len) {
            inode_t* mid = inode_new_static(str, len);
            if (!mid)
                return NULL;

            size_t tail = child->prefix_len - cpl;
            char* np = malloc(tail);
            if (!np) {
                inode_free(mid);
                return NULL;
            }
            memcpy(np, child->prefix + cpl, tail);
            free(child->prefix);
            child->prefix = np;
            child->prefix_len = tail;

            mid->children = malloc(sizeof(inode_t*));
            if (!mid->children) {
                inode_free(mid);
                return NULL;
            }
            mid->children[0] = child;
            mid->n_children = 1;
            mid->cap_children = 1;
            node->children[i] = mid;
            return mid;
        }

        inode_t* mid = inode_new_static(str, cpl);
        if (!mid)
            return NULL;

        size_t ctail = child->prefix_len - cpl;
        char* cp = malloc(ctail);
        if (!cp) {
            inode_free(mid);
            return NULL;
        }
        memcpy(cp, child->prefix + cpl, ctail);
        free(child->prefix);
        child->prefix = cp;
        child->prefix_len = ctail;

        inode_t* leaf = inode_new_static(str + cpl, len - cpl);
        if (!leaf) {
            inode_free(mid);
            return NULL;
        }

        mid->children = malloc(2 * sizeof(inode_t*));
        if (!mid->children) {
            inode_free(leaf);
            inode_free(mid);
            return NULL;
        }
        mid->children[0] = child;
        mid->children[1] = leaf;
        mid->n_children = 2;
        mid->cap_children = 2;
        node->children[i] = mid;
        return leaf;
    }

    inode_t* leaf = inode_new_static(str, len);
    if (!leaf)
        return NULL;
    if (!inode_add_child(node, leaf)) {
        inode_free(leaf);
        return NULL;
    }
    return leaf;
}

static inode_t* insert_param(inode_t* node, chxrt_param_type type) {
    for (size_t i = 0; i < node->n_children; i++) {
        if (!node->children[i]->is_param)
            return NULL;
        if (node->children[i]->param_type == type)
            return node->children[i];
        return NULL;
    }

    inode_t* child = inode_new_param(type);
    if (!child)
        return NULL;
    if (!inode_add_child(node, child)) {
        inode_free(child);
        return NULL;
    }
    return child;
}

static size_t inode_count(const inode_t* n) {
    size_t c = 1;
    for (size_t i = 0; i < n->n_children; i++)
        c += inode_count(n->children[i]);
    return c;
}

static size_t inode_prefix_bytes(const inode_t* n) {
    size_t s = n->prefix_len;
    for (size_t i = 0; i < n->n_children; i++)
        s += inode_prefix_bytes(n->children[i]);
    return s;
}

static int cmp_child_by_first_char(const void* a, const void* b) {
    const inode_t* na = *(const inode_t* const*)a;
    const inode_t* nb = *(const inode_t* const*)b;
    if (na->prefix_len == 0 && nb->prefix_len == 0)
        return 0;
    if (na->prefix_len == 0)
        return -1;
    if (nb->prefix_len == 0)
        return 1;
    return (int)(unsigned char)na->prefix[0] -
           (int)(unsigned char)nb->prefix[0];
}

static void sort_children_rec(inode_t* n) {
    if (n->n_children > 1) {
        bool all_static = true;
        for (size_t i = 0; i < n->n_children; i++) {
            if (n->children[i]->is_param) {
                all_static = false;
                break;
            }
        }
        if (all_static) {
            qsort(n->children, n->n_children, sizeof(inode_t*),
                  cmp_child_by_first_char);
        }
    }
    for (size_t i = 0; i < n->n_children; i++)
        sort_children_rec(n->children[i]);
}

static bool parse_int64(const char* s, size_t len, int64_t* out) {
    if (len == 0)
        return false;

    size_t pos = 0;
    bool neg = false;

    if (s[0] == '-') {
        neg = true;
        pos = 1;
    } else if (s[0] == '+') {
        pos = 1;
    }
    if (pos >= len)
        return false;

    const uint64_t abs_max =
        neg ? (uint64_t)INT64_MAX + 1ULL : (uint64_t)INT64_MAX;

    uint64_t val = 0;
    for (size_t i = pos; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        uint64_t d = (uint64_t)(s[i] - '0');
        if (val > (abs_max - d) / 10)
            return false;
        val = val * 10 + d;
    }

    if (neg) {
        if (val == (uint64_t)INT64_MAX + 1ULL)
            *out = INT64_MIN;
        else
            *out = -(int64_t)val;
    } else {
        *out = (int64_t)val;
    }
    return true;
}

static bool parse_uint64(const char* s, size_t len, uint64_t* out) {
    if (len == 0)
        return false;

    uint64_t val = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        uint64_t d = (uint64_t)(s[i] - '0');
        if (val > (UINT64_MAX - d) / 10)
            return false;
        val = val * 10 + d;
    }
    *out = val;
    return true;
}

struct chxrt_tree_st* chxrt_new(void) {
    chxrt_tree* t = calloc(1, sizeof(chxrt_tree));
    if (!t)
        return NULL;
    t->root = inode_new();
    if (!t->root) {
        free(t);
        return NULL;
    }
    return t;
}

int chxrt_insert(struct chxrt_tree_st* tree, const char* key, size_t key_len,
                 void* value) {
    if (tree->is_compiled)
        return -1;

    segment_t segs[MAX_SEGMENTS];
    int n = parse_pattern(key, key_len, segs, MAX_SEGMENTS);
    if (n < 0)
        return -1;

    inode_t* cur = tree->root;
    for (int i = 0; i < n; i++) {
        cur = segs[i].is_param ? insert_param(cur, segs[i].param_type)
                               : insert_static(cur, segs[i].str, segs[i].len);
        if (!cur)
            return -1;
    }

    if (cur->is_leaf)
        return -1;

    cur->is_leaf = true;
    cur->user_data = value;
    return 0;
}

static inode_t* find_static(inode_t* node, const char* str, size_t len) {
    if (len == 0)
        return node;

    for (size_t i = 0; i < node->n_children; i++) {
        if (node->children[i]->is_param)
            return NULL;
    }

    for (size_t i = 0; i < node->n_children; i++) {
        inode_t* child = node->children[i];
        size_t cpl =
            prefix_common_len(child->prefix, child->prefix_len, str, len);
        if (cpl == 0)
            continue;

        if (cpl == child->prefix_len && cpl == len)
            return child;

        if (cpl == child->prefix_len)
            return find_static(child, str + cpl, len - cpl);

        return NULL;
    }

    return NULL;
}

static inode_t* find_param(inode_t* node, chxrt_param_type type) {
    for (size_t i = 0; i < node->n_children; i++) {
        if (!node->children[i]->is_param)
            return NULL;
        if (node->children[i]->param_type == type)
            return node->children[i];
        return NULL;
    }
    return NULL;
}

int chxrt_find(const struct chxrt_tree_st* tree, const char* key,
               size_t key_len, void** out) {
    if (tree->is_compiled)
        return -1;

    segment_t segs[MAX_SEGMENTS];
    int n = parse_pattern(key, key_len, segs, MAX_SEGMENTS);
    if (n < 0)
        return -1;

    inode_t* cur = tree->root;
    for (int i = 0; i < n; i++) {
        cur = segs[i].is_param ? find_param(cur, segs[i].param_type)
                               : find_static(cur, segs[i].str, segs[i].len);
        if (!cur)
            return -1;
    }

    if (!cur->is_leaf)
        return -1;

    *out = &cur->user_data;
    return 0;
}

int chxrt_acquire(const struct chxrt_tree_st* tree, const char* key,
                  size_t key_len, void** out) {
    if (tree->is_compiled)
        return -1;

    struct chxrt_tree_st* mtree = (struct chxrt_tree_st*)tree;

    segment_t segs[MAX_SEGMENTS];
    int n = parse_pattern(key, key_len, segs, MAX_SEGMENTS);
    if (n < 0)
        return -1;

    inode_t* cur = mtree->root;
    for (int i = 0; i < n; i++) {
        cur = segs[i].is_param ? insert_param(cur, segs[i].param_type)
                               : insert_static(cur, segs[i].str, segs[i].len);
        if (!cur)
            return -1;
    }

    int is_new = !cur->is_leaf;
    cur->is_leaf = true;
    *out = &cur->user_data;
    return is_new;
}

int chxrt_compile(struct chxrt_tree_st* tree, size_t* heap_alloc_bytes) {
    if (heap_alloc_bytes)
        *heap_alloc_bytes = 0;

    if (tree->is_compiled)
        return -1;

    sort_children_rec(tree->root);

    size_t total = inode_count(tree->root);
    size_t pool_sz = inode_prefix_bytes(tree->root);

    if (total > SIZE_MAX / sizeof(cnode_t))
        return -1;
    if (total > SIZE_MAX / sizeof(inode_t*))
        return -1;

    size_t compiled_bytes = total * sizeof(cnode_t);
    size_t queue_bytes = total * sizeof(inode_t*);

    if (compiled_bytes > SIZE_MAX - pool_sz)
        return -1;
    size_t total_heap_bytes = compiled_bytes + pool_sz;
    if (total_heap_bytes > SIZE_MAX - queue_bytes)
        return -1;
    total_heap_bytes += queue_bytes;

    cnode_t* compiled = calloc(total, sizeof(cnode_t));
    if (!compiled)
        return -1;
    char* pool = pool_sz ? malloc(pool_sz) : NULL;
    if (pool_sz && !pool) {
        free(compiled);
        return -1;
    }

    inode_t** queue = malloc(total * sizeof(inode_t*));
    if (!queue) {
        free(compiled);
        free(pool);
        return -1;
    }
    queue[0] = tree->root;
    size_t head = 0, tail = 1;

    while (head < tail) {
        inode_t* cur = queue[head++];
        for (size_t i = 0; i < cur->n_children; i++)
            queue[tail++] = cur->children[i];
    }

    size_t pool_off = 0;
    uint32_t child_off = 1;

    for (size_t i = 0; i < total; i++) {
        inode_t* nd = queue[i];

        compiled[i].is_param = nd->is_param;
        compiled[i].is_leaf = nd->is_leaf;
        compiled[i].param_type = nd->param_type;
        compiled[i].user_data = nd->user_data;
        compiled[i].n_children = (uint32_t)nd->n_children;
        compiled[i].first_child = child_off;
        child_off += (uint32_t)nd->n_children;

        if (nd->prefix_len) {
            memcpy(pool + pool_off, nd->prefix, nd->prefix_len);
            compiled[i].prefix_offset = (uint32_t)pool_off;
            compiled[i].prefix_len = (uint32_t)nd->prefix_len;
            pool_off += nd->prefix_len;
        }
    }

    free(queue);

    inode_free(tree->root);
    tree->root = NULL;

    tree->compiled = compiled;
    tree->n_compiled = total;
    tree->string_pool = pool;
    tree->is_compiled = true;
    if (heap_alloc_bytes)
        *heap_alloc_bytes = total_heap_bytes;
    return 0;
}

int chxrt_lookup(const struct chxrt_tree_st* tree, const char* key,
                 size_t key_len, void** out, struct chxrt_param_st* params,
                 size_t params_n) {
    if (__builtin_expect(!tree->is_compiled, 0))
        return -1;

    const cnode_t* nodes = tree->compiled;
    const char* pool = tree->string_pool;
    size_t cur = 0;
    size_t pos = 0;
    size_t pidx = 0;

    while (pos < key_len) {
        const cnode_t* nd = &nodes[cur];
        const uint16_t nc = nd->n_children;

        if (__builtin_expect(nc == 0, 0))
            return -1;

        const uint32_t first = nd->first_child;
        const cnode_t* fc = &nodes[first];

        if (__builtin_expect(!fc->is_param, 1)) {
            const unsigned char target = (unsigned char)key[pos];

            if (__builtin_expect(nc == 1, 1)) {
                const char* prefix = pool + fc->prefix_offset;
                if (__builtin_expect((unsigned char)prefix[0] != target, 0))
                    return -1;
                size_t rem = key_len - pos;
                if (__builtin_expect(rem < fc->prefix_len, 0))
                    return -1;
                if (__builtin_expect(
                        faster_memcmp(key + pos, prefix, fc->prefix_len) != 0,
                        0))
                    return -1;
                pos += fc->prefix_len;
                cur = first;
                continue;
            }

            uint32_t matched_idx = 0;
            bool found = false;
            for (uint16_t ci = 0; ci < nc; ci++) {
                uint32_t idx = first + ci;
                const char* prefix = pool + nodes[idx].prefix_offset;
                unsigned char fb = (unsigned char)prefix[0];
                if (fb < target)
                    continue;
                if (__builtin_expect(fb > target, 0))
                    return -1;
                matched_idx = idx;
                found = true;
                break;
            }

            if (__builtin_expect(!found, 0))
                return -1;

            const cnode_t* ch = &nodes[matched_idx];
            const char* prefix = pool + ch->prefix_offset;
            size_t rem = key_len - pos;
            if (__builtin_expect(rem < ch->prefix_len, 0))
                return -1;
            if (__builtin_expect(
                    faster_memcmp(key + pos, prefix, ch->prefix_len) != 0, 0))
                return -1;

            pos += ch->prefix_len;
            cur = matched_idx;
            continue;
        }

        size_t seg_start = pos;
        while (pos < key_len && key[pos] != '/')
            pos++;
        size_t seg_len = pos - seg_start;

        if (__builtin_expect(seg_len == 0, 0))
            return -1;

        if (__builtin_expect(pidx >= params_n, 0))
            return -2;

        switch (fc->param_type) {
        case CHXRT_PARAM_INT: {
            int64_t v;
            if (__builtin_expect(!parse_int64(key + seg_start, seg_len, &v), 0))
                return -3;
            params[pidx].type = CHXRT_PARAM_INT;
            params[pidx].int_value = v;
            break;
        }
        case CHXRT_PARAM_UINT: {
            uint64_t v;
            if (__builtin_expect(!parse_uint64(key + seg_start, seg_len, &v),
                                 0))
                return -3;
            params[pidx].type = CHXRT_PARAM_UINT;
            params[pidx].uint_value = v;
            break;
        }
        case CHXRT_PARAM_STR:
            params[pidx].type = CHXRT_PARAM_STR;
            params[pidx].str_begin = key + seg_start;
            params[pidx].str_end = key + pos;
            break;
        default:
            return -1;
        }
        pidx++;
        cur = first;
        continue;
    }

    if (__builtin_expect(!nodes[cur].is_leaf, 0))
        return -1;

    *out = nodes[cur].user_data;
    return 0;
}

void chxrt_delete(struct chxrt_tree_st* tree) {
    if (!tree)
        return;
    if (tree->is_compiled) {
        free(tree->compiled);
        free(tree->string_pool);
    } else {
        inode_free(tree->root);
    }
    free(tree);
}
