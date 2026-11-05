# Host tests for the portable core: plain C11 under ASan and UBSan.
#   make -C mcu/esp-idf/test/host test
ROOT := ../..
CC ?= cc
CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pedantic -Wshadow \
	-Wconversion -Wno-sign-conversion -g -O1 \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
	-I$(ROOT)/alloy_storage/portable/include
SOURCES := $(wildcard $(ROOT)/alloy_storage/portable/*.c) test_main.c
TARGET ?= $(CURDIR)/alloy-portable-tests

.PHONY: test clean
test:
	$(CC) $(CFLAGS) $(SOURCES) -o $(TARGET) -lm
	$(TARGET)

clean:
	rm -f $(TARGET)
