CC = gcc

OUT = file_server

SRC = file_server.c

all: $(OUT)
	$(CC) $(SRC) -o $(OUT) -pthread

$(OUT): $(SRC)
	$(CC) $(SRC) -o $(OUT) -pthread

clean:
	rm -f $(OUT)	

# Default watch dir if not provided
WATCH_DIR ?= public

run: $(OUT)
	./$(OUT) $(WATCH_DIR)