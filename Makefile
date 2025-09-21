CC = gcc
CFLAGS = -O3 -march=native -mtune=native -flto -Wall -Wextra -std=gnu11 -D_GNU_SOURCE
CFLAGS += -ffast-math -funroll-loops -finline-functions -fomit-frame-pointer
CFLAGS += -DNDEBUG -pipe -pthread
LDFLAGS = -lpthread -lrt -flto
TARGET = ultra_server
SRCDIR = src
INCDIR = include
OBJDIR = obj

SOURCES = $(wildcard $(SRCDIR)/*.c)
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)
HEADERS = $(wildcard $(INCDIR)/*.h)

.PHONY: all clean run benchmark profile

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c $(HEADERS) | $(OBJDIR)
	$(CC) $(CFLAGS) -I$(INCDIR) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(TARGET)

run: $(TARGET)
	sudo ./$(TARGET)

benchmark: $(TARGET)
	@echo "Running benchmark..."
	@./benchmark.sh

profile: $(TARGET)
	sudo perf record -g ./$(TARGET) &
	sleep 5
	curl -s http://localhost:8080/ > /dev/null
	sudo pkill $(TARGET)
	sudo perf report