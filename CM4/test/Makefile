# The shims stand in for the board and come first on the include path, so a
# change to the real Core/Inc/main.h does not reach this suite. That is the
# trade: the test is about what phy_poll does with a part, not about the wiring
# to one. radio_devices_docs/open_hub/testing/host-tests.md
CFLAGS = -std=c11 -Wall -Wextra -Werror -O1 -g \
         -I shim -I ../Core/Inc -I ../rfm69_lib -I ../rfm69_lib/test \
         -I ../../radio_stack/inc -I ../../radio_stack/profiles \
         -DRADIO_PROFILE=RADIO_PROFILE_ASBUILT

TESTS = test_phy test_timebase

test_phy: test_phy.c fake_part.h ../Core/Src/phy_rfm69.c ../rfm69_lib/rfm69.c \
          ../../radio_stack/inc/phy.h ../Core/Inc/phy_rfm69.h
	$(CC) $(CFLAGS) -o $@ test_phy.c ../Core/Src/phy_rfm69.c ../rfm69_lib/rfm69.c

# The real timebase.c, not a model of it: this is the file the grid's every
# interval is converted by, and it had no host test at all.
test_timebase: test_timebase.c ../Core/Src/timebase.c ../Core/Inc/clock.h \
               ../../radio_stack/inc/timebase.h
	$(CC) $(CFLAGS) -o $@ test_timebase.c ../Core/Src/timebase.c

.PHONY: check clean
check: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

clean:
	rm -f $(TESTS)
