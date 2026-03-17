#include "chx/rt.h"

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const size_t SAMPLES_N = 10000;
static const uint32_t MISS_RATE_PERCENT = 10;

struct result {
    int pattern_id;

    int params_n;
    struct chxrt_param_st params[4];
};

enum parsed_token_kind {
    PARSED_TOKEN_LITERAL,
    PARSED_TOKEN_INT,
    PARSED_TOKEN_UINT,
    PARSED_TOKEN_STR,
};

struct parsed_token {
    enum parsed_token_kind kind;
    const char* begin;
    size_t len;
};

struct sample {
    char path[128];
    size_t path_len;
    int should_miss;
    struct result expected;
};

struct pattern_input {
    const char* method;
    const char* path;
};

struct pattern;

struct pattern {
    int pattern_id;
    const char* raw_pattern;
    char normalized_pattern[256];
    const char* pattern;
    const char* method;
    int enabled;
    int tokens_n;
    struct parsed_token tokens[8];
};

static uint32_t random_u32(void) {
    uint32_t hi = (uint32_t)(rand() & 0xffff);
    uint32_t lo = (uint32_t)(rand() & 0xffff);
    return (hi << 16) | lo;
}

static int32_t random_i32(void) {
    return (int32_t)(random_u32() % 2000001u) - 1000000;
}

static uint32_t random_u32_range(uint32_t max_inclusive) {
    return max_inclusive == 0 ? 0 : (random_u32() % (max_inclusive + 1));
}

static size_t append_literal(char* buffer, size_t offset, const char* s) {
    const size_t len = strlen(s);
    memcpy(buffer + offset, s, len);
    return offset + len;
}

static size_t append_int(char* buffer, size_t offset, int64_t value) {
    int written = snprintf(buffer + offset, 128 - offset, "%" PRId64, value);
    assert(written > 0);
    return offset + (size_t)written;
}

static size_t append_uint(char* buffer, size_t offset, uint64_t value) {
    int written = snprintf(buffer + offset, 128 - offset, "%" PRIu64, value);
    assert(written > 0);
    return offset + (size_t)written;
}

static size_t append_random_word(char* buffer, size_t offset, size_t len,
                                 const char** begin_out, const char** end_out) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";

    *begin_out = buffer + offset;
    for (size_t i = 0; i < len; ++i) {
        buffer[offset + i] = alphabet[random_u32_range(sizeof(alphabet) - 2)];
    }
    *end_out = buffer + offset + len;
    return offset + len;
}

static int parse_pattern_tokens(struct pattern* pattern) {
    const char* p = pattern->raw_pattern;
    int tokens_n = 0;

    while (*p != '\0') {
        const char* seg_begin = p;
        const char* seg_end = strchr(p, '/');
        size_t seg_len;

        if (seg_end == NULL) {
            seg_end = p + strlen(p);
        }
        seg_len = (size_t)(seg_end - seg_begin);

        assert(tokens_n <
               (int)(sizeof(pattern->tokens) / sizeof(pattern->tokens[0])));

        if (seg_len >= 2 && seg_begin[0] == ':') {
            pattern->tokens[tokens_n].kind = PARSED_TOKEN_STR;
            pattern->tokens[tokens_n].begin = seg_begin;
            pattern->tokens[tokens_n].len = seg_len;
            ++tokens_n;
        } else if (seg_len == strlen("<int>") &&
                   memcmp(seg_begin, "<int>", strlen("<int>")) == 0) {
            pattern->tokens[tokens_n].kind = PARSED_TOKEN_INT;
            pattern->tokens[tokens_n].begin = seg_begin;
            pattern->tokens[tokens_n].len = seg_len;
            ++tokens_n;
        } else if (seg_len == strlen("<uint>") &&
                   memcmp(seg_begin, "<uint>", strlen("<uint>")) == 0) {
            pattern->tokens[tokens_n].kind = PARSED_TOKEN_UINT;
            pattern->tokens[tokens_n].begin = seg_begin;
            pattern->tokens[tokens_n].len = seg_len;
            ++tokens_n;
        } else if (seg_len == strlen("<str>") &&
                   memcmp(seg_begin, "<str>", strlen("<str>")) == 0) {
            pattern->tokens[tokens_n].kind = PARSED_TOKEN_STR;
            pattern->tokens[tokens_n].begin = seg_begin;
            pattern->tokens[tokens_n].len = seg_len;
            ++tokens_n;
        } else {
            pattern->tokens[tokens_n].kind = PARSED_TOKEN_LITERAL;
            pattern->tokens[tokens_n].begin = seg_begin;
            pattern->tokens[tokens_n].len = seg_len;
            ++tokens_n;
        }

        if (*seg_end == '\0') {
            break;
        }

        p = seg_end + 1;
    }

    pattern->tokens_n = tokens_n;
    return 0;
}

static void normalize_pattern_for_insert(struct pattern* pattern) {
    const char* src = pattern->raw_pattern;
    size_t src_len = strlen(src);
    size_t si = 0;
    size_t di = 0;

    while (si < src_len) {
        if (src[si] == ':') {
            size_t seg_end = si + 1;
            while (seg_end < src_len && src[seg_end] != '/') {
                ++seg_end;
            }

            assert(di + strlen("<str>") < sizeof(pattern->normalized_pattern));
            memcpy(pattern->normalized_pattern + di, "<str>", strlen("<str>"));
            di += strlen("<str>");
            si = seg_end;
            continue;
        }

        assert(di + 1 < sizeof(pattern->normalized_pattern));
        pattern->normalized_pattern[di++] = src[si++];
    }

    pattern->normalized_pattern[di] = '\0';
    pattern->pattern = pattern->normalized_pattern;
}

static size_t append_literal_span(char* buffer, size_t offset,
                                  const char* begin, size_t len,
                                  int add_leading_slash) {
    if (add_leading_slash) {
        buffer[offset++] = '/';
    }
    memcpy(buffer + offset, begin, len);
    return offset + len;
}

static int pattern_has_static_literal(const struct pattern* pattern) {
    for (int i = 0; i < pattern->tokens_n; ++i) {
        const struct parsed_token* token = &pattern->tokens[i];
        if (token->kind == PARSED_TOKEN_LITERAL && token->len > 0) {
            return 1;
        }
    }
    return 0;
}

static void generate_sample_from_pattern(struct sample* sample,
                                         const struct pattern* pattern) {
    size_t off = 0;
    int params_n = 0;

    sample->expected.pattern_id = pattern->pattern_id;

    for (int i = 0; i < pattern->tokens_n; ++i) {
        const struct parsed_token* token = &pattern->tokens[i];
        if (i > 0 || (token->len > 0 && token->begin[0] != '/')) {
            sample->path[off++] = '/';
        }

        switch (token->kind) {
        case PARSED_TOKEN_LITERAL:
            off = append_literal_span(sample->path, off, token->begin,
                                      token->len, 0);
            break;
        case PARSED_TOKEN_INT: {
            const int64_t value = random_i32();
            off = append_int(sample->path, off, value);
            sample->expected.params[params_n].type = CHXRT_PARAM_INT;
            sample->expected.params[params_n].int_value = value;
            ++params_n;
            break;
        }
        case PARSED_TOKEN_UINT: {
            const uint64_t value = (uint64_t)random_u32();
            off = append_uint(sample->path, off, value);
            sample->expected.params[params_n].type = CHXRT_PARAM_UINT;
            sample->expected.params[params_n].uint_value = value;
            ++params_n;
            break;
        }
        case PARSED_TOKEN_STR: {
            const size_t len = (size_t)random_u32_range(7) + 3;
            const char* str_begin = NULL;
            const char* str_end = NULL;
            off = append_random_word(sample->path, off, len, &str_begin,
                                     &str_end);
            sample->expected.params[params_n].type = CHXRT_PARAM_STR;
            sample->expected.params[params_n].str_begin = str_begin;
            sample->expected.params[params_n].str_end = str_end;
            ++params_n;
            break;
        }
        default:
            assert(0);
        }
    }

    sample->path[off] = '\0';
    sample->path_len = off;
    sample->expected.params_n = params_n;
}

static void mutate_sample_to_miss(struct sample* sample) {
    assert(sample->path_len > 0);
    sample->path[sample->path_len - 1] =
        sample->path[sample->path_len - 1] == '0' ? '1' : '0';
    sample->path[sample->path_len] = '\0';
    sample->should_miss = 1;
}

static void mutate_sample_to_miss_by_pattern(struct sample* sample,
                                             const struct pattern* pattern) {
    size_t off = 0;
    int mutated = 0;

    for (int i = 0; i < pattern->tokens_n; ++i) {
        const struct parsed_token* token = &pattern->tokens[i];
        size_t seg_begin;

        if (i > 0 || (token->len > 0 && token->begin[0] != '/')) {
            sample->path[off++] = '/';
        }
        seg_begin = off;

        switch (token->kind) {
        case PARSED_TOKEN_LITERAL:
            off = append_literal_span(sample->path, off, token->begin,
                                      token->len, 0);
            if (!mutated && token->len > 0) {
                sample->path[seg_begin + token->len - 1] =
                    sample->path[seg_begin + token->len - 1] == '0' ? '1' : '0';
                mutated = 1;
            }
            break;
        case PARSED_TOKEN_INT: {
            const int64_t value = random_i32();
            off = append_int(sample->path, off, value);
            sample->expected.params[0].type = sample->expected.params[0].type;
            break;
        }
        case PARSED_TOKEN_UINT: {
            const uint64_t value = (uint64_t)random_u32();
            off = append_uint(sample->path, off, value);
            break;
        }
        case PARSED_TOKEN_STR: {
            const size_t len = (size_t)random_u32_range(7) + 3;
            const char* str_begin = NULL;
            const char* str_end = NULL;
            off = append_random_word(sample->path, off, len, &str_begin,
                                     &str_end);
            break;
        }
        default:
            assert(0);
        }
    }

    assert(mutated);
    sample->path[off] = '\0';
    sample->path_len = off;
    sample->should_miss = 1;
}

static const struct pattern_input pattern_inputs[] = {
    // OAuth Authorizations
    {"GET", "/authorizations"},
    {"GET", "/authorizations/:id"},
    {"POST", "/authorizations"},
    //{"PUT", "/authorizations/clients/:client_id"},
    //{"PATCH", "/authorizations/:id"},
    {"DELETE", "/authorizations/:id"},
    {"GET", "/applications/:client_id/tokens/:access_token"},
    {"DELETE", "/applications/:client_id/tokens"},
    {"DELETE", "/applications/:client_id/tokens/:access_token"},

    // Activity
    {"GET", "/events"},
    {"GET", "/repos/:owner/:repo/events"},
    {"GET", "/networks/:owner/:repo/events"},
    {"GET", "/orgs/:org/events"},
    {"GET", "/users/:user/received_events"},
    {"GET", "/users/:user/received_events/public"},
    {"GET", "/users/:user/events"},
    {"GET", "/users/:user/events/public"},
    {"GET", "/users/:user/events/orgs/:org"},
    {"GET", "/feeds"},
    {"GET", "/notifications"},
    {"GET", "/repos/:owner/:repo/notifications"},
    {"PUT", "/notifications"},
    {"PUT", "/repos/:owner/:repo/notifications"},
    {"GET", "/notifications/threads/:id"},
    //{"PATCH", "/notifications/threads/:id"},
    {"GET", "/notifications/threads/:id/subscription"},
    {"PUT", "/notifications/threads/:id/subscription"},
    {"DELETE", "/notifications/threads/:id/subscription"},
    {"GET", "/repos/:owner/:repo/stargazers"},
    {"GET", "/users/:user/starred"},
    {"GET", "/user/starred"},
    {"GET", "/user/starred/:owner/:repo"},
    {"PUT", "/user/starred/:owner/:repo"},
    {"DELETE", "/user/starred/:owner/:repo"},
    {"GET", "/repos/:owner/:repo/subscribers"},
    {"GET", "/users/:user/subscriptions"},
    {"GET", "/user/subscriptions"},
    {"GET", "/repos/:owner/:repo/subscription"},
    {"PUT", "/repos/:owner/:repo/subscription"},
    {"DELETE", "/repos/:owner/:repo/subscription"},
    {"GET", "/user/subscriptions/:owner/:repo"},
    {"PUT", "/user/subscriptions/:owner/:repo"},
    {"DELETE", "/user/subscriptions/:owner/:repo"},

    // Gists
    {"GET", "/users/:user/gists"},
    {"GET", "/gists"},
    //{"GET", "/gists/public"},
    //{"GET", "/gists/starred"},
    {"GET", "/gists/:id"},
    {"POST", "/gists"},
    //{"PATCH", "/gists/:id"},
    {"PUT", "/gists/:id/star"},
    {"DELETE", "/gists/:id/star"},
    {"GET", "/gists/:id/star"},
    {"POST", "/gists/:id/forks"},
    {"DELETE", "/gists/:id"},

    // Git Data
    {"GET", "/repos/:owner/:repo/git/blobs/:sha"},
    {"POST", "/repos/:owner/:repo/git/blobs"},
    {"GET", "/repos/:owner/:repo/git/commits/:sha"},
    {"POST", "/repos/:owner/:repo/git/commits"},
    //{"GET", "/repos/:owner/:repo/git/refs/*ref"},
    {"GET", "/repos/:owner/:repo/git/refs"},
    {"POST", "/repos/:owner/:repo/git/refs"},
    //{"PATCH", "/repos/:owner/:repo/git/refs/*ref"},
    //{"DELETE", "/repos/:owner/:repo/git/refs/*ref"},
    {"GET", "/repos/:owner/:repo/git/tags/:sha"},
    {"POST", "/repos/:owner/:repo/git/tags"},
    {"GET", "/repos/:owner/:repo/git/trees/:sha"},
    {"POST", "/repos/:owner/:repo/git/trees"},

    // Issues
    {"GET", "/issues"},
    {"GET", "/user/issues"},
    {"GET", "/orgs/:org/issues"},
    {"GET", "/repos/:owner/:repo/issues"},
    {"GET", "/repos/:owner/:repo/issues/:number"},
    {"POST", "/repos/:owner/:repo/issues"},
    //{"PATCH", "/repos/:owner/:repo/issues/:number"},
    {"GET", "/repos/:owner/:repo/assignees"},
    {"GET", "/repos/:owner/:repo/assignees/:assignee"},
    {"GET", "/repos/:owner/:repo/issues/:number/comments"},
    //{"GET", "/repos/:owner/:repo/issues/comments"},
    //{"GET", "/repos/:owner/:repo/issues/comments/:id"},
    {"POST", "/repos/:owner/:repo/issues/:number/comments"},
    //{"PATCH", "/repos/:owner/:repo/issues/comments/:id"},
    //{"DELETE", "/repos/:owner/:repo/issues/comments/:id"},
    {"GET", "/repos/:owner/:repo/issues/:number/events"},
    //{"GET", "/repos/:owner/:repo/issues/events"},
    //{"GET", "/repos/:owner/:repo/issues/events/:id"},
    {"GET", "/repos/:owner/:repo/labels"},
    {"GET", "/repos/:owner/:repo/labels/:name"},
    {"POST", "/repos/:owner/:repo/labels"},
    //{"PATCH", "/repos/:owner/:repo/labels/:name"},
    {"DELETE", "/repos/:owner/:repo/labels/:name"},
    {"GET", "/repos/:owner/:repo/issues/:number/labels"},
    {"POST", "/repos/:owner/:repo/issues/:number/labels"},
    {"DELETE", "/repos/:owner/:repo/issues/:number/labels/:name"},
    {"PUT", "/repos/:owner/:repo/issues/:number/labels"},
    {"DELETE", "/repos/:owner/:repo/issues/:number/labels"},
    {"GET", "/repos/:owner/:repo/milestones/:number/labels"},
    {"GET", "/repos/:owner/:repo/milestones"},
    {"GET", "/repos/:owner/:repo/milestones/:number"},
    {"POST", "/repos/:owner/:repo/milestones"},
    //{"PATCH", "/repos/:owner/:repo/milestones/:number"},
    {"DELETE", "/repos/:owner/:repo/milestones/:number"},

    // Miscellaneous
    {"GET", "/emojis"},
    {"GET", "/gitignore/templates"},
    {"GET", "/gitignore/templates/:name"},
    {"POST", "/markdown"},
    {"POST", "/markdown/raw"},
    {"GET", "/meta"},
    {"GET", "/rate_limit"},

    // Organizations
    {"GET", "/users/:user/orgs"},
    {"GET", "/user/orgs"},
    {"GET", "/orgs/:org"},
    //{"PATCH", "/orgs/:org"},
    {"GET", "/orgs/:org/members"},
    {"GET", "/orgs/:org/members/:user"},
    {"DELETE", "/orgs/:org/members/:user"},
    {"GET", "/orgs/:org/public_members"},
    {"GET", "/orgs/:org/public_members/:user"},
    {"PUT", "/orgs/:org/public_members/:user"},
    {"DELETE", "/orgs/:org/public_members/:user"},
    {"GET", "/orgs/:org/teams"},
    {"GET", "/teams/:id"},
    {"POST", "/orgs/:org/teams"},
    //{"PATCH", "/teams/:id"},
    {"DELETE", "/teams/:id"},
    {"GET", "/teams/:id/members"},
    {"GET", "/teams/:id/members/:user"},
    {"PUT", "/teams/:id/members/:user"},
    {"DELETE", "/teams/:id/members/:user"},
    {"GET", "/teams/:id/repos"},
    {"GET", "/teams/:id/repos/:owner/:repo"},
    {"PUT", "/teams/:id/repos/:owner/:repo"},
    {"DELETE", "/teams/:id/repos/:owner/:repo"},
    {"GET", "/user/teams"},

    // Pull Requests
    {"GET", "/repos/:owner/:repo/pulls"},
    {"GET", "/repos/:owner/:repo/pulls/:number"},
    {"POST", "/repos/:owner/:repo/pulls"},
    //{"PATCH", "/repos/:owner/:repo/pulls/:number"},
    {"GET", "/repos/:owner/:repo/pulls/:number/commits"},
    {"GET", "/repos/:owner/:repo/pulls/:number/files"},
    {"GET", "/repos/:owner/:repo/pulls/:number/merge"},
    {"PUT", "/repos/:owner/:repo/pulls/:number/merge"},
    {"GET", "/repos/:owner/:repo/pulls/:number/comments"},
    //{"GET", "/repos/:owner/:repo/pulls/comments"},
    //{"GET", "/repos/:owner/:repo/pulls/comments/:number"},
    {"PUT", "/repos/:owner/:repo/pulls/:number/comments"},
    //{"PATCH", "/repos/:owner/:repo/pulls/comments/:number"},
    //{"DELETE", "/repos/:owner/:repo/pulls/comments/:number"},

    // Repositories
    {"GET", "/user/repos"},
    {"GET", "/users/:user/repos"},
    {"GET", "/orgs/:org/repos"},
    {"GET", "/repositories"},
    {"POST", "/user/repos"},
    {"POST", "/orgs/:org/repos"},
    {"GET", "/repos/:owner/:repo"},
    //{"PATCH", "/repos/:owner/:repo"},
    {"GET", "/repos/:owner/:repo/contributors"},
    {"GET", "/repos/:owner/:repo/languages"},
    {"GET", "/repos/:owner/:repo/teams"},
    {"GET", "/repos/:owner/:repo/tags"},
    {"GET", "/repos/:owner/:repo/branches"},
    {"GET", "/repos/:owner/:repo/branches/:branch"},
    {"DELETE", "/repos/:owner/:repo"},
    {"GET", "/repos/:owner/:repo/collaborators"},
    {"GET", "/repos/:owner/:repo/collaborators/:user"},
    {"PUT", "/repos/:owner/:repo/collaborators/:user"},
    {"DELETE", "/repos/:owner/:repo/collaborators/:user"},
    {"GET", "/repos/:owner/:repo/comments"},
    {"GET", "/repos/:owner/:repo/commits/:sha/comments"},
    {"POST", "/repos/:owner/:repo/commits/:sha/comments"},
    {"GET", "/repos/:owner/:repo/comments/:id"},
    //{"PATCH", "/repos/:owner/:repo/comments/:id"},
    {"DELETE", "/repos/:owner/:repo/comments/:id"},
    {"GET", "/repos/:owner/:repo/commits"},
    {"GET", "/repos/:owner/:repo/commits/:sha"},
    {"GET", "/repos/:owner/:repo/readme"},
    //{"GET", "/repos/:owner/:repo/contents/*path"},
    //{"PUT", "/repos/:owner/:repo/contents/*path"},
    //{"DELETE", "/repos/:owner/:repo/contents/*path"},
    //{"GET", "/repos/:owner/:repo/:archive_format/:ref"},
    {"GET", "/repos/:owner/:repo/keys"},
    {"GET", "/repos/:owner/:repo/keys/:id"},
    {"POST", "/repos/:owner/:repo/keys"},
    //{"PATCH", "/repos/:owner/:repo/keys/:id"},
    {"DELETE", "/repos/:owner/:repo/keys/:id"},
    {"GET", "/repos/:owner/:repo/downloads"},
    {"GET", "/repos/:owner/:repo/downloads/:id"},
    {"DELETE", "/repos/:owner/:repo/downloads/:id"},
    {"GET", "/repos/:owner/:repo/forks"},
    {"POST", "/repos/:owner/:repo/forks"},
    {"GET", "/repos/:owner/:repo/hooks"},
    {"GET", "/repos/:owner/:repo/hooks/:id"},
    {"POST", "/repos/:owner/:repo/hooks"},
    //{"PATCH", "/repos/:owner/:repo/hooks/:id"},
    {"POST", "/repos/:owner/:repo/hooks/:id/tests"},
    {"DELETE", "/repos/:owner/:repo/hooks/:id"},
    {"POST", "/repos/:owner/:repo/merges"},
    {"GET", "/repos/:owner/:repo/releases"},
    {"GET", "/repos/:owner/:repo/releases/:id"},
    {"POST", "/repos/:owner/:repo/releases"},
    //{"PATCH", "/repos/:owner/:repo/releases/:id"},
    {"DELETE", "/repos/:owner/:repo/releases/:id"},
    {"GET", "/repos/:owner/:repo/releases/:id/assets"},
    {"GET", "/repos/:owner/:repo/stats/contributors"},
    {"GET", "/repos/:owner/:repo/stats/commit_activity"},
    {"GET", "/repos/:owner/:repo/stats/code_frequency"},
    {"GET", "/repos/:owner/:repo/stats/participation"},
    {"GET", "/repos/:owner/:repo/stats/punch_card"},
    {"GET", "/repos/:owner/:repo/statuses/:ref"},
    {"POST", "/repos/:owner/:repo/statuses/:ref"},

    // Search
    {"GET", "/search/repositories"},
    {"GET", "/search/code"},
    {"GET", "/search/issues"},
    {"GET", "/search/users"},
    {"GET", "/legacy/issues/search/:owner/:repository/:state/:keyword"},
    {"GET", "/legacy/repos/search/:keyword"},
    {"GET", "/legacy/user/search/:keyword"},
    {"GET", "/legacy/user/email/:email"},

    // Users
    {"GET", "/users/:user"},
    {"GET", "/user"},
    //{"PATCH", "/user"},
    {"GET", "/users"},
    {"GET", "/user/emails"},
    {"POST", "/user/emails"},
    {"DELETE", "/user/emails"},
    {"GET", "/users/:user/followers"},
    {"GET", "/user/followers"},
    {"GET", "/users/:user/following"},
    {"GET", "/user/following"},
    {"GET", "/user/following/:user"},
    {"GET", "/users/:user/following/:target_user"},
    {"PUT", "/user/following/:user"},
    {"DELETE", "/user/following/:user"},
    {"GET", "/users/:user/keys"},
    {"GET", "/user/keys"},
    {"GET", "/user/keys/:id"},
    {"POST", "/user/keys"},
    //{"PATCH", "/user/keys/:id"},
    {"DELETE", "/user/keys/:id"},
};

static const struct pattern* pattern_from_value(void* value) {
    return (const struct pattern*)value;
}

static int compare_param(const struct chxrt_param_st* actual,
                         const struct chxrt_param_st* expected) {
    if (actual->type != expected->type) {
        return -1;
    }

    switch (actual->type) {
    case CHXRT_PARAM_INT:
        return actual->int_value == expected->int_value ? 0 : -1;
    case CHXRT_PARAM_UINT:
        return actual->uint_value == expected->uint_value ? 0 : -1;
    case CHXRT_PARAM_STR: {
        const size_t actual_len = (size_t)(actual->str_end - actual->str_begin);
        const size_t expected_len =
            (size_t)(expected->str_end - expected->str_begin);
        return actual_len == expected_len &&
                       memcmp(actual->str_begin, expected->str_begin,
                              actual_len) == 0
                   ? 0
                   : -1;
    }
    default:
        return -1;
    }
}

static int compare_result(const struct result* expected, void* out,
                          const struct chxrt_param_st* actual_params) {
    const struct pattern* actual_pattern = pattern_from_value(out);
    if (actual_pattern == NULL ||
        actual_pattern->pattern_id != expected->pattern_id) {
        return -1;
    }

    for (int i = 0; i < expected->params_n; ++i) {
        if (compare_param(&actual_params[i], &expected->params[i]) != 0) {
            return -1;
        }
    }

    return 0;
}

static uint64_t elapsed_ns(const struct timeval* begin,
                           const struct timeval* end) {
    const uint64_t sec = (uint64_t)(end->tv_sec - begin->tv_sec);
    const uint64_t usec = (uint64_t)(end->tv_usec - begin->tv_usec);
    return sec * 1000000000ull + usec * 1000ull;
}

static void dump_pattern(const struct pattern* pattern) {
    printf("pattern_id=%d method=%s path=%s enabled=%d tokens_n=%d\n",
           pattern->pattern_id, pattern->method, pattern->pattern,
           pattern->enabled, pattern->tokens_n);
}

static void dump_sample(const struct sample* sample) {
    printf("sample path=%s path_len=%zu should_miss=%d expected_pattern_id=%d "
           "params_n=%d\n",
           sample->path, sample->path_len, sample->should_miss,
           sample->expected.pattern_id, sample->expected.params_n);
    for (int i = 0; i < sample->expected.params_n; ++i) {
        const struct chxrt_param_st* p = &sample->expected.params[i];
        switch (p->type) {
        case CHXRT_PARAM_INT:
            printf("  expected param[%d]=INT:%" PRId64 "\n", i, p->int_value);
            break;
        case CHXRT_PARAM_UINT:
            printf("  expected param[%d]=UINT:%" PRIu64 "\n", i, p->uint_value);
            break;
        case CHXRT_PARAM_STR:
            printf("  expected param[%d]=STR:%.*s\n", i,
                   (int)(p->str_end - p->str_begin), p->str_begin);
            break;
        default:
            printf("  expected param[%d]=UNKNOWN\n", i);
            break;
        }
    }
}

static void dump_actual_params(const struct chxrt_param_st* params,
                               int params_n) {
    for (int i = 0; i < params_n; ++i) {
        const struct chxrt_param_st* p = &params[i];
        switch (p->type) {
        case CHXRT_PARAM_INT:
            printf("  actual param[%d]=INT:%" PRId64 "\n", i, p->int_value);
            break;
        case CHXRT_PARAM_UINT:
            printf("  actual param[%d]=UINT:%" PRIu64 "\n", i, p->uint_value);
            break;
        case CHXRT_PARAM_STR:
            printf("  actual param[%d]=STR:%.*s\n", i,
                   (int)(p->str_end - p->str_begin), p->str_begin);
            break;
        default:
            printf("  actual param[%d]=UNKNOWN(type=%d)\n", i, p->type);
            break;
        }
    }
}

int main(void) {
    chxrt_tree* tree = chxrt_new();
    struct pattern* patterns = NULL;
    struct pattern** active_patterns = NULL;
    struct sample* samples = NULL;
    struct result* actual_results = NULL;
    void** actual_values = NULL;
    struct timeval ts_begin;
    struct timeval ts_end;
    size_t active_pattern_n = 0;
    size_t skipped_pattern_n = 0;
    assert(tree);

    patterns = (struct pattern*)malloc(
        sizeof(*patterns) *
        (sizeof(pattern_inputs) / sizeof(pattern_inputs[0])));
    active_patterns = (struct pattern**)malloc(
        sizeof(*active_patterns) *
        (sizeof(pattern_inputs) / sizeof(pattern_inputs[0])));
    samples = (struct sample*)malloc(sizeof(*samples) * SAMPLES_N);
    actual_results =
        (struct result*)malloc(sizeof(*actual_results) * SAMPLES_N);
    actual_values = (void**)malloc(sizeof(*actual_values) * SAMPLES_N);
    assert(patterns != NULL);
    assert(active_patterns != NULL);
    assert(samples != NULL);
    assert(actual_results != NULL);
    assert(actual_values != NULL);

    srand(1);

    const size_t pattern_n = sizeof(pattern_inputs) / sizeof(pattern_inputs[0]);
    for (size_t i = 0; i < pattern_n; ++i) {
        struct pattern* pattern = &patterns[i];
        pattern->pattern_id = (int)(i + 1);
        pattern->method = pattern_inputs[i].method;
        pattern->raw_pattern = pattern_inputs[i].path;
        normalize_pattern_for_insert(pattern);
        pattern->enabled = 0;
        pattern->tokens_n = 0;
        memset(pattern->tokens, 0, sizeof(pattern->tokens));
        assert(parse_pattern_tokens(pattern) == 0);
        if (chxrt_insert(tree, pattern->pattern, strlen(pattern->pattern),
                         (void*)pattern) == 0) {
            pattern->enabled = 1;
            active_patterns[active_pattern_n++] = pattern;
        } else {
            ++skipped_pattern_n;
        }
    }

    assert(active_pattern_n > 0);

    size_t allocated_n = 0;
    assert(chxrt_compile(tree, &allocated_n) == 0);
    printf("allocated %zu bytes\n", allocated_n);

    for (size_t i = 0; i < SAMPLES_N; ++i) {
        const struct pattern* pattern =
            active_patterns[random_u32_range((uint32_t)active_pattern_n - 1)];
        if (random_u32_range(99) < MISS_RATE_PERCENT &&
            pattern_has_static_literal(pattern)) {
            generate_sample_from_pattern(&samples[i], pattern);
            mutate_sample_to_miss_by_pattern(&samples[i], pattern);
        } else {
            generate_sample_from_pattern(&samples[i], pattern);
            samples[i].should_miss = 0;
        }
        memset(&actual_results[i], 0, sizeof(actual_results[i]));
        actual_values[i] = NULL;
    }

    assert(gettimeofday(&ts_begin, NULL) == 0);
    for (size_t i = 0; i < SAMPLES_N; ++i) {
        const int lookup_ret =
            chxrt_lookup(tree, samples[i].path, samples[i].path_len,
                         &actual_values[i], actual_results[i].params,
                         sizeof(actual_results[i].params) /
                             sizeof(actual_results[i].params[0]));

        if (samples[i].should_miss) {
            if (lookup_ret != -1) {
                printf("unexpected hit for miss sample #%zu ret=%d\n", i,
                       lookup_ret);
                dump_sample(&samples[i]);
                if (actual_values[i] != NULL) {
                    const struct pattern* p =
                        pattern_from_value(actual_values[i]);
                    printf("matched pattern for miss sample:\n");
                    dump_pattern(p);
                }
                dump_actual_params(actual_results[i].params,
                                   samples[i].expected.params_n);
            }
            assert(lookup_ret == -1);
            actual_results[i].pattern_id = -1;
            actual_results[i].params_n = 0;
        } else {
            if (lookup_ret != 0) {
                printf("lookup failed at sample #%zu ret=%d\n", i, lookup_ret);
                dump_sample(&samples[i]);
                dump_pattern(active_patterns[0]);
                if (actual_values[i] != NULL) {
                    const struct pattern* p =
                        pattern_from_value(actual_values[i]);
                    printf("actual matched pattern candidate:\n");
                    dump_pattern(p);
                }
                dump_actual_params(actual_results[i].params,
                                   samples[i].expected.params_n);
            }
            assert(lookup_ret == 0);
            actual_results[i].pattern_id =
                pattern_from_value(actual_values[i])->pattern_id;
            actual_results[i].params_n = samples[i].expected.params_n;
        }
    }
    assert(gettimeofday(&ts_end, NULL) == 0);

    for (size_t i = 0; i < SAMPLES_N; ++i) {
        if (samples[i].should_miss) {
            assert(actual_values[i] == NULL);
            assert(actual_results[i].pattern_id == -1);
            assert(actual_results[i].params_n == 0);
        } else {
            assert(compare_result(&samples[i].expected, actual_values[i],
                                  actual_results[i].params) == 0);
        }
    }

    {
        const uint64_t total_ns = elapsed_ns(&ts_begin, &ts_end);
        const double avg_ns = (double)total_ns / (double)SAMPLES_N;
        printf("param_bench: patterns_total=%zu patterns_active=%zu "
               "patterns_skipped=%zu\n",
               pattern_n, active_pattern_n, skipped_pattern_n);
        printf("param_bench: samples=%zu miss_rate=%u%% total_ns=%" PRIu64
               " avg_ns=%.2f\n",
               SAMPLES_N, MISS_RATE_PERCENT, total_ns, avg_ns);
        printf("param_bench: verification ok\n");
    }

    free(active_patterns);
    free(actual_values);
    free(actual_results);
    free(samples);
    free(patterns);
    chxrt_delete(tree);
    return 0;
}