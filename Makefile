CC       ?= gcc
TARGET   := bin/logana-engine
LIBTTAK  := lib/libttak/lib/libttak.a

SRCDIRS  := . src src/core src/analysis src/render src/server src/utils
INCDIRS  := . include lib/libttak/include

CFLAGS   ?= -O3 -std=c17 -Wall -Wextra -Wshadow -pthread -D_GNU_SOURCE
CPPFLAGS += $(addprefix -I,$(INCDIRS))
LDFLAGS  += -pthread -lm

SRC      := $(sort $(foreach d,$(SRCDIRS),$(wildcard $(d)/*.c)))
OBJ      := $(patsubst %.c,obj/%.o,$(SRC))
DEP      := $(OBJ:.o=.d)

.PHONY: all clean dirs libttak print-src

all: $(TARGET)

libttak:
	$(MAKE) -C lib/libttak clean
	$(MAKE) -C lib/libttak

$(TARGET): libttak dirs $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ) $(LIBTTAK) -o $@ $(LDFLAGS)

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
