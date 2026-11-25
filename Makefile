# Makefile - Estacion de Telemetria ITLA
#
#   make            compila todo y deja el programa en build/estacion
#   make run        compila y ejecuta (por defecto "config"; otro: make run ARGS=enviar)
#   make clean      borra la carpeta build/ (NO borra estacion.db)
#   make distclean  borra build/ y tambien la base estacion.db
#
# Funciona en Linux (Raspberry Pi OS / Ubuntu) y en Windows con MSYS2 UCRT64.
# Requiere: gcc, make y libcurl (Ubuntu/Pi: sudo apt install libcurl4-openssl-dev;
# MSYS2: pacman -S mingw-w64-ucrt-x86_64-curl).

CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -g -Ilib/sqlite -MMD -MP
LDLIBS  = -lcurl -lm

# SQLite se compila aparte, una sola vez, con sus propias opciones
SQLITE_FLAGS = -O2 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION

BUILD  = build
TARGET = $(BUILD)/estacion
SRC    = src/main.c src/comandos.c src/red.c src/datos.c src/sensor.c
OBJ    = $(SRC:src/%.c=$(BUILD)/%.o)
SQLITE = $(BUILD)/sqlite3.o

ARGS ?= config

.PHONY: all run clean distclean

all: $(TARGET)

$(TARGET): $(OBJ) $(SQLITE)
	$(CC) $^ -o $@ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(SQLITE): lib/sqlite/sqlite3.c | $(BUILD)
	$(CC) $(SQLITE_FLAGS) -Ilib/sqlite -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

run: all
	./$(TARGET) $(ARGS)

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -f estacion.db estacion.db-wal estacion.db-shm

-include $(OBJ:.o=.d)
