/* Linux harness: the guest tick counter is a variable the test sets (the real one reads the host clock). */
#include <stdint.h>
uint64_t mock_ticks;
uint64_t runtime_guest_ticks(void) { return mock_ticks; }
