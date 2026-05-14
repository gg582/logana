CC       ?= gcc
TARGET   := bin/logana-engine
LIBTTAK  := lib/libttak/lib/libttak.a
CWIST_REPO ?= https://github.com/religiya-serdtsa/cwist.git
CWIST_REF  ?= main
CWIST_ROOT ?= .deps/cwist
CWIST_LIB  := $(CWIST_ROOT)/libcwist.a
CWIST_CJSON := $(CWIST_ROOT)/lib/cjson/libcjson.a
CWIST_URIPARSER := $(CWIST_ROOT)/lib/uriparser/build/liburiparser.a
CWIST_STAMP := $(CWIST_ROOT)/.logana-cwist-ready
CWIST_SYSTEM_SQLITE ?= 0
CWIST_SQLITE_STAMP := $(CWIST_ROOT)/.logana-cwist-sqlite-ready

SRCDIRS  := . src src/core src/analysis src/render src/server src/utils
INCDIRS  := . include lib/libttak/include $(CWIST_ROOT)/include $(CWIST_ROOT)/lib

CFLAGS   ?= -O3 -std=c17 -Wall -Wextra -Wshadow -pthread -D_GNU_SOURCE
CPPFLAGS += $(addprefix -I,$(INCDIRS))
LDFLAGS  += -pthread -lm -lsqlite3 -lssl -lcrypto -ldl

SRC      := $(sort $(foreach d,$(SRCDIRS),$(wildcard $(d)/*.c)))
OBJ      := $(patsubst %.c,obj/%.o,$(SRC))
DEP      := $(OBJ:.o=.d)

.PHONY: all clean dirs libttak cwist prepare-cwist print-src

all: $(TARGET)

libttak:
	$(MAKE) -C lib/libttak clean
	$(MAKE) -C lib/libttak

prepare-cwist: $(CWIST_STAMP) $(CWIST_SQLITE_STAMP)

$(CWIST_STAMP):
	@mkdir -p $(dir $@)
	@if [ ! -d "$(CWIST_ROOT)/.git" ]; then \
		echo "Cloning cwist from $(CWIST_REPO) ($(CWIST_REF))..."; \
		git clone --depth=1 --branch "$(CWIST_REF)" "$(CWIST_REPO)" "$(CWIST_ROOT)"; \
	fi
	@echo "Preparing cwist dependencies..."
	@git -C "$(CWIST_ROOT)" submodule update --init --depth=1 lib/cjson lib/uriparser lib/libttak
	@touch $@

$(CWIST_SQLITE_STAMP): $(CWIST_STAMP)
	@if [ "$(CWIST_SYSTEM_SQLITE)" = "1" ]; then \
		echo "Configuring cwist to link against system SQLite..."; \
		CWIST_MAKEFILE="$(CWIST_ROOT)/Makefile" python3 -c 'from pathlib import Path; import os; path = Path(os.environ["CWIST_MAKEFILE"]); text = path.read_text(); dollar = chr(36); text = text.replace(" -I./lib/sqlite3", ""); text = text if " -lsqlite3" in text else text.replace(" -lttak\n", " -lttak -lsqlite3\n", 1); all_line = "all: " + " ".join([dollar + "(LIBTTAK_LIB)", dollar + "(CJSON_LIB)", dollar + "(URIPARSER_LIB)", dollar + "(LIB_NAME)"]) + "\n"; lines = []; [lines.append(all_line if line.startswith("all:") and "sqlite3.c" in line else line) for line in text.splitlines(True) if line != "       lib/sqlite3/sqlite3.c \\\n"]; path.write_text("".join(lines))'; \
	fi
	@touch $@

cwist: prepare-cwist
	$(MAKE) -C $(CWIST_ROOT)

$(TARGET): libttak cwist dirs $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ) $(CWIST_LIB) $(LIBTTAK) $(CWIST_CJSON) $(CWIST_URIPARSER) -o $@ $(LDFLAGS)

obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

dirs:
	@mkdir -p obj $(dir $(TARGET))

clean:
	rm -rf obj $(TARGET)
	$(MAKE) -C lib/libttak clean

print-src:
	@printf '%s\n' $(SRC)

-include $(DEP)
