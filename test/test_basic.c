#include "chx/rt.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static void test_static_lookup(void) {
    chxrt_tree* tree = chxrt_new();
    assert(tree != NULL);

    int marker = 42;
    void* out = NULL;

    assert(chxrt_insert(tree, "/hello", strlen("/hello"), &marker) == 0);
    assert(chxrt_compile(tree, NULL) == 0);
    assert(chxrt_lookup(tree, "/hello", strlen("/hello"), &out, NULL, 0) == 0);
    assert(out == &marker);
    assert(chxrt_lookup(tree, "/not-found", strlen("/not-found"), &out, NULL,
                        0) == -1);

    chxrt_delete(tree);
}

static void test_lookup_requires_compile(void) {
    chxrt_tree* tree = chxrt_new();
    assert(tree != NULL);

    int marker = 7;
    void* out = NULL;

    assert(chxrt_insert(tree, "/raw", strlen("/raw"), &marker) == 0);
    assert(chxrt_lookup(tree, "/raw", strlen("/raw"), &out, NULL, 0) == -1);

    chxrt_delete(tree);
}

static void test_insert_and_compile_errors(void) {
    chxrt_tree* tree = chxrt_new();
    assert(tree != NULL);

    int a = 1;
    int b = 2;

    assert(chxrt_insert(tree, "/dup", strlen("/dup"), &a) == 0);
    assert(chxrt_insert(tree, "/dup", strlen("/dup"), &b) == -1);
    assert(chxrt_insert(tree, "/bad/<float>", strlen("/bad/<float>"), &a) ==
           -1);
    assert(chxrt_insert(tree, "/bad/<int", strlen("/bad/<int"), &a) == -1);

    assert(chxrt_compile(tree, NULL) == 0);
    assert(chxrt_compile(tree, NULL) == -1);
    assert(chxrt_insert(tree, "/after-compile", strlen("/after-compile"), &a) ==
           -1);

    chxrt_delete(tree);
}

static void test_conflict_rules(void) {
    chxrt_tree* tree = chxrt_new();
    assert(tree != NULL);

    int a = 11;
    int b = 12;
    int c = 13;
    int d = 14;

    assert(chxrt_insert(tree, "/x/fixed", strlen("/x/fixed"), &a) == 0);
    assert(chxrt_insert(tree, "/x/<str>", strlen("/x/<str>"), &b) == -1);

    assert(chxrt_insert(tree, "/p/<int>", strlen("/p/<int>"), &c) == 0);
    assert(chxrt_insert(tree, "/p/<uint>", strlen("/p/<uint>"), &d) == -1);

    chxrt_delete(tree);
}

static void test_radix_prefix_splitting(void) {
    chxrt_tree* tree = chxrt_new();
    assert(tree != NULL);

    int hello = 21;
    int helium = 22;
    void* out = NULL;

    assert(chxrt_insert(tree, "/hello", strlen("/hello"), &hello) == 0);
    assert(chxrt_insert(tree, "/helium", strlen("/helium"), &helium) == 0);
    assert(chxrt_compile(tree, NULL) == 0);

    assert(chxrt_lookup(tree, "/hello", strlen("/hello"), &out, NULL, 0) == 0);
    assert(out == &hello);
    assert(chxrt_lookup(tree, "/helium", strlen("/helium"), &out, NULL, 0) ==
           0);
    assert(out == &helium);
    assert(chxrt_lookup(tree, "/hel", strlen("/hel"), &out, NULL, 0) == -1);

    chxrt_delete(tree);
}

static void test_lookup_without_endpoint(void) {
    chxrt_tree* tree = chxrt_new();
    assert(tree != NULL);

    int abc = 31;
    int abd = 32;
    void* out = NULL;

    assert(chxrt_insert(tree, "/abc", strlen("/abc"), &abc) == 0);
    assert(chxrt_insert(tree, "/abd", strlen("/abd"), &abd) == 0);
    assert(chxrt_compile(tree, NULL) == 0);

    assert(chxrt_lookup(tree, "/ab", strlen("/ab"), &out, NULL, 0) == -1);

    chxrt_delete(tree);
}

static void test_param_lookup_success(void) {
    chxrt_tree* tree = chxrt_new();
    assert(tree != NULL);

    int marker = 99;
    void* out = NULL;
    struct chxrt_param_st params[3];
    const char* path = "/users/-7/posts/42/name/alice";

    assert(chxrt_insert(tree, "/users/<int>/posts/<uint>/name/<str>",
                        strlen("/users/<int>/posts/<uint>/name/<str>"),
                        &marker) == 0);
    assert(chxrt_compile(tree, NULL) == 0);

    assert(chxrt_lookup(tree, path, strlen(path), &out, params, 3) == 0);
    assert(out == &marker);

    assert(params[0].type == CHXRT_PARAM_INT);
    assert(params[0].int_value == -7);
    assert(params[1].type == CHXRT_PARAM_UINT);
    assert(params[1].uint_value == 42);
    assert(params[2].type == CHXRT_PARAM_STR);
    assert((size_t)(params[2].str_end - params[2].str_begin) == 5);
    assert(memcmp(params[2].str_begin, "alice", 5) == 0);

    chxrt_delete(tree);
}

static void test_param_lookup_errors(void) {
    chxrt_tree* tree = chxrt_new();
    assert(tree != NULL);

    int marker = 123;
    void* out = NULL;
    struct chxrt_param_st params[2];

    assert(chxrt_insert(tree, "/a/<int>/b/<uint>", strlen("/a/<int>/b/<uint>"),
                        &marker) == 0);
    assert(chxrt_compile(tree, NULL) == 0);

    assert(chxrt_lookup(tree, "/a/7/b/9", strlen("/a/7/b/9"), &out, params,
                        1) == -2);
    assert(chxrt_lookup(tree, "/a/nope/b/9", strlen("/a/nope/b/9"), &out,
                        params, 2) == -3);
    assert(chxrt_lookup(tree, "/a/7/b/-9", strlen("/a/7/b/-9"), &out, params,
                        2) == -3);

    chxrt_delete(tree);
}

static void test_delete_null(void) { chxrt_delete(NULL); }

int main(void) {
    test_static_lookup();
    test_lookup_requires_compile();
    test_insert_and_compile_errors();
    test_conflict_rules();
    test_radix_prefix_splitting();
    test_lookup_without_endpoint();
    test_param_lookup_success();
    test_param_lookup_errors();
    test_delete_null();

    return 0;
}
