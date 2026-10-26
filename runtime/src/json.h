/* Minimal JSON reader for the data directory's manifest (objects, arrays, strings, numbers, bools, null). */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct JsonNode {
    enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } type;
    char *str; /* J_STR value; J_OBJ member key for children */
    double num;
    struct JsonNode *child, *next;
    char *key;
} JsonNode;

JsonNode *json_parse(const char *text, size_t len); /* NULL on malformed input */
const JsonNode *json_get(const JsonNode *obj, const char *key);
const char *json_str(const JsonNode *obj, const char *key, const char *fallback);
double json_num(const JsonNode *obj, const char *key, double fallback);
void json_free(JsonNode *n);
