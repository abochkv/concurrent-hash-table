CC := cc
CPPFLAGS := -D_POSIX_C_SOURCE=200809L -I.
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -g -O2
LDFLAGS :=
LDLIBS := -pthread -lrt

ifneq ($(SANITIZE),)
CFLAGS += -fsanitize=$(SANITIZE) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SANITIZE)
endif

.PHONY: all clean test stress

all: server client

server: build/server.o build/hash_table.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

client: build/client.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/hash_table_test: build/tests/hash_table_test.o build/hash_table.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: %.c protocol.h hash_table.h | build
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p $@

test: all build/hash_table_test
	./build/hash_table_test
	sh tests/integration.sh

stress: all
	sh tests/stress.sh

clean:
	rm -rf build server client
