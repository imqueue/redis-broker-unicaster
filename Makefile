# Compiler and flags
CC = gcc
CFLAGS = -fPIC -Wall -Wextra -O2 -lpthread
REDIS_INCLUDE_PATH = .

# Module name and files
MODULE_NAME = unicaster
SRC = $(MODULE_NAME).c
SO = $(MODULE_NAME).so

LIBS = -luuid -lcurl -ljson-c

# Default rule: Compile the shared library
all: $(SO)

# Compile the Redis module
$(SO): $(SRC) redismodule.h
	$(CC) $(CFLAGS) -shared -o $(SO) $(SRC) -I$(REDIS_INCLUDE_PATH) $(LIBS)

# Clean up build artifacts
clean:
	rm -f $(SO) dump.rdb

# Install module to a system-wide directory (Optional)
install: $(SO)
	mkdir -p /usr/local/lib/redis_modules
	cp $(SO) /usr/local/lib/redis_modules/

# Uninstall the module
uninstall:
	rm -f /usr/local/lib/redis_modules/$(SO)

# Reload Redis with the module
reload:
	redis-cli MODULE UNLOAD unicaster || true
	redis-cli MODULE LOAD $(SO)

# Run Redis with the module (for testing)
run:
	redis-server --loadmodule $(PWD)/$(SO)

.PHONY: all clean install uninstall reload run
