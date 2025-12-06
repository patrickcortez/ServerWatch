CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
TARGET = file_server
SRCS = main.c net.c client.c file_ops.c metadata.c
OBJS = $(SRCS:.c=.o)
HEADERS = common.h net.h client.h file_ops.h metadata.h

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

run: $(TARGET)
	./$(TARGET) $(filter-out $@,$(MAKECMDGOALS))

%:
	@:

.PHONY: all clean run