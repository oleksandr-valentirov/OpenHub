# The library's own suite is radio_stack/test; this one holds what is this
# hub's alone - the mailbox, the store and the northbound codec.
CFLAGS = -std=c11 -Wall -Wextra -Werror -O1 -g -I../inc -I vectors \
         -I../../radio_stack/inc -I../../radio_stack/profiles \
         -DRADIO_PROFILE=RADIO_PROFILE_ASBUILT

# The vector headers are listed explicitly, and that is the whole point of this
# comment. Without them a republished vector set does not rebuild anything: the
# binaries are up to date against their own sources, every test passes, and they
# are testing a contract that has moved. It is the "include the headers, do not
# copy them" rule defeated through the build system instead of through a stale
# copy - which is why neither side's rule covered it. Found by the device side,
# whose suite went green printing digests three sets old.
#
# Not -MMD: each test is one cc invocation over several sources, every source
# writes the same .d, and only the last one's headers survive. It looks like
# dependency tracking and tracks one translation unit.
# Named, and named LIVE: this list watched wire_v3 and pair_v2 long after the
# build stopped including either, so the guard against a moved contract was
# itself pointed at files that could no longer move.
VECTORS = vectors/oht_v1.h

test_ipc: test_ipc.c ../inc/ipc.h
	$(CC) $(CFLAGS) -o $@ test_ipc.c

test_oht: test_oht.c ../src/oht_codec.c ../inc/oht_proto.h ../inc/oht_fields.h $(VECTORS)
	$(CC) $(CFLAGS) -o $@ test_oht.c ../src/oht_codec.c

test_cfg: test_cfg.c ../src/cfgjournal.c ../inc/cfgjournal.h ../inc/cfgstore.h
	$(CC) $(CFLAGS) -o $@ test_cfg.c ../src/cfgjournal.c

.PHONY: check clean
check: test_ipc test_oht test_cfg
	./test_ipc
	./test_oht
	./test_cfg

clean:
	rm -f test_ipc test_oht test_cfg
