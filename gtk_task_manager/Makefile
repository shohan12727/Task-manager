CC = gcc
CFLAGS = -Wall -Wextra -O2
GTK_FLAGS = $(shell pkg-config --cflags --libs gtk+-3.0)

task_manager.exe: src/main.c
	$(CC) $(CFLAGS) src/main.c -o task_manager.exe $(GTK_FLAGS) -lpsapi

clean:
	rm -f task_manager.exe
