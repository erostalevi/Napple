/* libFuzzer entry for one target: clang -fsanitize=fuzzer,address,undefined (make fuzz-build builds build/fuzz/<name>_lf). */
#include "fuzz.h"
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fuzz_target.run(data, size);
    return 0;
}
const char *__asan_default_options(void) { return "detect_leaks=0:max_allocation_size_mb=512:allocator_may_return_null=0"; }
