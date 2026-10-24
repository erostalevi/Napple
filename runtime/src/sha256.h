#pragma once
#include <stddef.h>
#include <stdint.h>
void sha256(const void *data, size_t n, uint8_t out[32]);
