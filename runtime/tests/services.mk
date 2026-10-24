# Unit tests for the services added in services-2026-10-24 (included from the Makefile). `make test-services` builds and runs every
# tests/test_svc_*.c. Each test #includes the service source it exercises (to reach its static handlers), so that source's object is
# left out of the link; the portable *_core.c objects link normally.
SVC_TESTS := $(patsubst tests/%.c,$(BUILD)/tests/%,$(wildcard tests/test_svc_*.c))
svc_included = $(patsubst %.c,$(BUILD)/%.o,$(shell grep -o 'services/[A-Za-z0-9_]*\.c' $(1) | sort -u))
$(BUILD)/tests/test_svc_%: tests/test_svc_%.c tests/check.h tests/svc_test.h $(BUILD)/tests/stubs.o $(OBJ)
	$(CC) $(CFLAGS) $< $(BUILD)/tests/stubs.o $(filter-out $(call svc_included,$<),$(TESTOBJ)) $(GFX_LIBS) $(FRAMEWORKS) $(EXTRA_LIBS) -o $@
test-services: $(SVC_TESTS)
	@for t in $^; do case $$t in *.o) continue;; esac; echo "== $$t"; ./$$t || exit 1; done
.PHONY: test-services

# macOS-only: hid handlers backed by hid_core (hid.c needs Mach and the input library, so there is no Linux build of this one).
$(BUILD)/tests/test_hid_depth: tests/test_hid_depth.c tests/check.h $(BUILD)/tests/stubs.o $(OBJ)
	$(CC) $(CFLAGS) tests/test_hid_depth.c $(BUILD)/tests/stubs.o $(filter-out $(BUILD)/services/hid.o,$(TESTOBJ)) $(GFX_LIBS) $(FRAMEWORKS) $(EXTRA_LIBS) -o $@
test-services: $(BUILD)/tests/test_hid_depth
