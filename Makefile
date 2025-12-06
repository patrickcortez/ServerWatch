CC = gcc
IN = file_server.c
OUT = file_server
DEPS = -pthread

all:
	$(CC) $(IN) -o $(OUT) $(DEPS)

clean:
	rm -f $(OUT)

run: all
	./$(OUT) $(filter-out $@,$(MAKECMDGOALS))

%:
	@:
