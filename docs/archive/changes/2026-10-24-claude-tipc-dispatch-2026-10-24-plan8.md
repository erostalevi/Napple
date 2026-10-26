# TIPC dispatch (runtime/src/ipc.c)

TIPC requests (message type >= 16, command = type - 16) used to hard-fail with "TIPC not implemented". They are now served:
raw argument words follow the descriptors (no CMIF header), target is the session's plain object (no domains), and the reply is
special header/pointer descriptors, then raw words: result code first, then output data. Handles and new objects use the HIPC paths.

Verified: `make -C Napple/runtime test test-services test-host` pass (new TIPC checks in tests/test_ipc.c).
Animal Crossing (3.0.3 build) passes the TIPC point at ~0.48 s; next blocker is ICommonStateGetter cmd 900 (applet, not this area).
