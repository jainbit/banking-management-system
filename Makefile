# Banking Management System Makefile
# Works on macOS (clang via the `gcc` shim from Xcode Command Line Tools) and Linux.

CC      = gcc
CFLAGS  = -g -std=gnu11 -Wall -Wextra -pthread
LDFLAGS = -pthread

# Source files
SERVER_SRC = server.c signals.c auth.c records.c customer.c employee.c manager.c \
             admin.c utils.c session_manager.c
CLIENT_SRC = client.c
HEADERS    = $(wildcard *.h)

# Executables
SERVER = server
CLIENT = client

# Default target
all: $(SERVER) $(CLIENT)
	@echo "✓ Build complete!"

# Build server (rebuilds when any .c in the list or any header changes)
$(SERVER): $(SERVER_SRC) $(HEADERS)
	$(CC) $(CFLAGS) $(SERVER_SRC) -o $(SERVER) $(LDFLAGS)

# Build client
$(CLIENT): $(CLIENT_SRC) common.h
	$(CC) $(CFLAGS) $(CLIENT_SRC) -o $(CLIENT)

# Run server / client
run-server: $(SERVER)
	./$(SERVER)

run-client: $(CLIENT)
	./$(CLIENT)

# Remove build outputs (and macOS debug-symbol folders, leftover temp files)
clean:
	rm -rf *.o $(SERVER) $(CLIENT) *.dSYM data/*.tmp

# Remove the System V shared memory + semaphore if the server was killed
# with `kill -9` and could not clean up after itself.
reset-ipc:
	-ipcrm -M 9999 2>/dev/null; ipcrm -S 10000 2>/dev/null; true
	@echo "✓ IPC objects removed (if any existed)"

# !!! WARNING: overwrites ALL data files with fresh sample data !!!
setup:
	@mkdir -p data
	@echo "id,username,password,balance,active" > data/customers.csv
	@echo "1,customer1,pass123,1000.00,1" >> data/customers.csv
	@echo "2,customer2,pass123,2000.00,1" >> data/customers.csv
	@echo "3,alice,alice123,5000.00,1" >> data/customers.csv
	@echo "4,bob,bob123,3000.00,1" >> data/customers.csv
	@echo "id,username,password" > data/employees.csv
	@echo "1,employee1,pass123" >> data/employees.csv
	@echo "id,username,password" > data/managers.csv
	@echo "1,manager1,pass123" >> data/managers.csv
	@echo "id,username,password" > data/admins.csv
	@echo "1,admin1,pass123" >> data/admins.csv
	@echo "customer_id,type,amount,balance_after,timestamp" > data/transactions.csv
	@echo "loan_id,customer_id,amount,loan_type,tenure,status,assigned_employee" > data/loans.csv
	@echo "feedback_id,customer_id,message,reviewed" > data/feedback.csv
	@echo "✓ Data setup complete"

# Rebuild
rebuild: clean all

.PHONY: all clean run-server run-client setup rebuild reset-ipc
