.PHONY: build dev clean compile

default: help

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O2
CPPFLAGS ?= -D_DEFAULT_SOURCE -Ivendor/md4c -Ivendor/mongoose
LDFLAGS ?=
LDLIBS ?= -pthread

BIN := huap
SRC := huap.c
VENDOR_MONGOOSE_SRC := vendor/mongoose/mongoose.c
VENDOR_MD4C_SRCS := vendor/md4c/md4c.c vendor/md4c/md4c-html.c vendor/md4c/entity.c

help:
	@echo "Usage: make [target]"
	@echo ""
	@echo "Targets:"
	@echo " 	build"
	@echo " 	dev"
	@echo " 	compile"
	@echo " 	clean"

build:
	@./build

dev:
	@./dev

compile:
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) $(VENDOR_MONGOOSE_SRC) $(VENDOR_MD4C_SRCS) $(LDFLAGS) $(LDLIBS) -o $(BIN)

clean:
	@rm -rf docs huap
