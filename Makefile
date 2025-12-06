CC = gcc
IN = file_server.c
OUT = file_server
LIBS = -pthread -lssl -lcrypto

all:
	$(CC) $(IN) -o $(OUT) $(LIBS)

cert:
	./generate_cert.sh

clean:
	rm -f $(OUT) server.key server.crt

run: all
	@if [ -z "$(filter-out $@,$(MAKECMDGOALS))" ]; then \
		echo "Usage: make run <directory> <password>"; \
	else \
		./$(OUT) $(filter-out $@,$(MAKECMDGOALS)); \
	fi

%:
	@:
