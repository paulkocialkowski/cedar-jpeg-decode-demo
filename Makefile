PROJECT = cedar-demo

BINARY = $(PROJECT)
SOURCES = cedar-demo.c
OBJECTS = $(SOURCES:.c=.o)
DEPENDS = $(SOURCES:.c=.d)

CC = gcc
CFLAGS =
LDFLAGS =

all: $(BINARY)

$(OBJECTS): %.o: %.c
	@echo " CC     $<"
	@$(CC) $(CFLAGS) -MMD -MF $*.d -c $< -o $@

$(BINARY): $(OBJECTS)
	@echo " LINK   $@"
	@$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LDFLAGS)

.PHONY: clean
clean:
	@echo " CLEAN"
	@rm -f $(OBJECTS)

-include $(DEPENDS)
