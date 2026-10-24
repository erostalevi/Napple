/* Tests that do not link module.c (no guest modules at all). */
#include "napple.h"
#include <stdio.h>
Module *g_modules;
unsigned g_nmodules;
const Module *module_by_name(const char *name) { (void)name; return NULL; }
const Module *module_for_address(uint64_t a) { (void)a; return NULL; }
void module_describe(uint64_t a, char *out, size_t n) { snprintf(out, n, "0x%llx", (unsigned long long)a); }
