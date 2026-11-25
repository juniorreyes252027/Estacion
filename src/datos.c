/* datos.c - Todo lo que toca SQLite (estacion.db) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "datos.h"

/* ================= conexion ================= */

/* Abre estacion.db y crea las tablas si no existen. Devuelve NULL si falla. */
sqlite3 *datos_abrir(void) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_FILE, &db) != SQLITE_OK) {
        fprintf(stderr, "No se pudo abrir %s: %s\n", DB_FILE, sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }
    /* Dos procesos (enviar y escuchar) comparten la base:
       WAL permite leer mientras otro escribe, y el timeout espera si esta ocupada. */
    sqlite3_busy_timeout(db, 2000);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    const char *sql =
        "CREATE TABLE IF NOT EXISTS config ("
        "  clave TEXT PRIMARY KEY, valor TEXT);"
        "CREATE TABLE IF NOT EXISTS lecturas_planta ("
        "  id INTEGER PRIMARY KEY, maquina TEXT, ts INTEGER,"
        "  temp REAL, corriente REAL, vibracion REAL, estado TEXT);"
        "CREATE TABLE IF NOT EXISTS telemetria ("
        "  seq INTEGER PRIMARY KEY, ts INTEGER, temp REAL, corriente REAL,"
        "  enviado INTEGER DEFAULT 0, intentos INTEGER DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS alarmas ("
        "  codigo TEXT PRIMARY KEY, maquina TEXT, ts_servidor INTEGER,"
        "  ts_recibida INTEGER, ts_ack INTEGER);";

    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "Error creando tablas: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

void datos_cerrar(sqlite3 *db) {
    if (db) sqlite3_close(db);
}

void datos_finalizar(sqlite3_stmt *st) {
    if (st) sqlite3_finalize(st);
}

/* Ejecuta un SELECT fijo que devuelve un solo numero (COUNT, SUM...). -1 si falla. */
long long datos_valor_entero(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    long long v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    if (sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* ================= transacciones ================= */

int datos_begin(sqlite3 *db) {
    if (sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "No se pudo iniciar la transaccion: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

int datos_commit(sqlite3 *db) {
    if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "No se pudo confirmar: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

void datos_rollback(sqlite3 *db) {
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
}

/* ================= tabla config ================= */

/* Guarda cada linea "clave=valor" del texto en la tabla config */
int datos_guardar_config(sqlite3 *db, char *texto) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO config (clave, valor) VALUES (?, ?);",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    char *linea = texto;
    while (linea && *linea) {
        char *fin = strchr(linea, '\n');
        if (fin) *fin = '\0';
        size_t largo = strlen(linea);
        if (largo > 0 && linea[largo - 1] == '\r') linea[largo - 1] = '\0';

        char *igual = strchr(linea, '=');
        if (igual) {
            *igual = '\0';                          /* separa clave y valor */
            sqlite3_reset(st);
            sqlite3_clear_bindings(st);
            sqlite3_bind_text(st, 1, linea, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, igual + 1, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) != SQLITE_DONE) {
                fprintf(stderr, "Error guardando config: %s\n", sqlite3_errmsg(db));
                sqlite3_finalize(st);
                return -1;
            }
        }
        linea = fin ? fin + 1 : NULL;
    }
    sqlite3_finalize(st);
    return 0;
}

/* Lee un valor de la tabla config. Devuelve 1 si lo encontro, 0 si no. */
int datos_leer_config(sqlite3 *db, const char *clave, char *salida, size_t n) {
    sqlite3_stmt *st = NULL;
    int hallado = 0;
    if (sqlite3_prepare_v2(db, "SELECT valor FROM config WHERE clave = ?;",
                           -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, clave, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(st, 0);
        if (v) {
            snprintf(salida, n, "%s", (const char *)v);
            hallado = 1;
        }
    }
    sqlite3_finalize(st);
    return hallado;
}

/* ================= tabla lecturas_planta ================= */

/* Si el campo esta vacio guarda NULL; si no, guarda el valor */
static void bind_texto(sqlite3_stmt *st, int i, const char *s) {
    if (s[0] == '\0') sqlite3_bind_null(st, i);
    else sqlite3_bind_text(st, i, s, -1, SQLITE_TRANSIENT);
}
static void bind_entero(sqlite3_stmt *st, int i, const char *s) {
    if (s[0] == '\0') sqlite3_bind_null(st, i);
    else sqlite3_bind_int64(st, i, strtoll(s, NULL, 10));
}
static void bind_real(sqlite3_stmt *st, int i, const char *s) {
    if (s[0] == '\0') sqlite3_bind_null(st, i);
    else sqlite3_bind_double(st, i, strtod(s, NULL));
}

/* Parte una linea CSV en campos, conservando los vacios.
   Cambia cada coma por '\0'. Devuelve cuantos campos encontro. */
static int separar(char *linea, char *campos[], int max) {
    int n = 0;
    char *inicio = linea;
    for (char *p = linea; ; p++) {
        if (*p == ',' || *p == '\0') {
            int es_final = (*p == '\0');
            *p = '\0';
            if (n < max) campos[n++] = inicio;
            if (es_final) break;
            inicio = p + 1;
        }
    }
    return n;
}

/* INSERT OR IGNORE: si el id ya existe, la fila repetida se ignora sin error */
sqlite3_stmt *datos_preparar_planta(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO lecturas_planta "
            "(id, maquina, ts, temp, corriente, vibracion, estado) "
            "VALUES (?, ?, ?, ?, ?, ?, ?);",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(db));
        return NULL;
    }
    return st;
}

/* Guarda las filas de una pagina CSV con la sentencia preparada ins.
   Devuelve cuantas filas leyo (-1 si hubo error) y deja en *max_id el id mas alto. */
int datos_guardar_pagina(sqlite3 *db, sqlite3_stmt *ins, char *csv, long long *max_id) {
    int filas = 0;
    int es_cabecera = 1;
    char *linea = csv;

    while (linea && *linea) {
        char *fin = strchr(linea, '\n');
        if (fin) *fin = '\0';
        size_t largo = strlen(linea);
        if (largo > 0 && linea[largo - 1] == '\r') linea[largo - 1] = '\0';

        if (es_cabecera) {
            es_cabecera = 0;                      /* la primera linea no es un dato */
        } else if (linea[0] != '\0') {
            char *c[7];
            if (separar(linea, c, 7) == 7 && c[0][0] != '\0') {
                long long id = strtoll(c[0], NULL, 10);
                sqlite3_reset(ins);
                sqlite3_clear_bindings(ins);
                sqlite3_bind_int64(ins, 1, id);
                bind_texto(ins, 2, c[1]);         /* maquina   */
                bind_entero(ins, 3, c[2]);        /* ts        */
                bind_real(ins, 4, c[3]);          /* temp      */
                bind_real(ins, 5, c[4]);          /* corriente */
                bind_real(ins, 6, c[5]);          /* vibracion */
                bind_texto(ins, 7, c[6]);         /* estado    */
                if (sqlite3_step(ins) != SQLITE_DONE) {
                    fprintf(stderr, "Error al insertar: %s\n", sqlite3_errmsg(db));
                    return -1;
                }
                filas++;
                if (id > *max_id) *max_id = id;
            }
        }
        linea = fin ? fin + 1 : NULL;
    }
    return filas;
}

long long datos_filas_unicas(sqlite3 *db) {
    return datos_valor_entero(db, "SELECT COUNT(*) FROM lecturas_planta;");
}

long long datos_suma_ids(sqlite3 *db) {
    return datos_valor_entero(db, "SELECT SUM(id) FROM lecturas_planta;");
}

/* Estadisticas por maquina, calculadas con SQL (Nivel 2).
   Una fila es valida si no le falta ningun campo, estado es 'OK' y temp no es -999.0.
   Llena out[] (hasta max maquinas) y devuelve cuantas hay, o -1 si falla. */
int datos_estadisticas(sqlite3 *db, double umbral, struct Estadistica *out, int max) {
    const char *sql =
        "WITH f AS ("
        "  SELECT maquina, temp,"
        "    COALESCE(ts IS NOT NULL AND temp IS NOT NULL"
        "      AND corriente IS NOT NULL AND vibracion IS NOT NULL"
        "      AND estado IS NOT NULL AND estado = 'OK'"
        "      AND temp <> -999.0, 0) AS valida"
        "  FROM lecturas_planta WHERE maquina IS NOT NULL) "
        "SELECT maquina,"
        "  SUM(valida),"
        "  COUNT(*) - SUM(valida),"
        "  MIN(CASE WHEN valida THEN temp END),"
        "  MAX(CASE WHEN valida THEN temp END),"
        "  ROUND(AVG(CASE WHEN valida THEN temp END), 2),"
        "  SUM(CASE WHEN valida AND temp > ? THEN 1 ELSE 0 END) "
        "FROM f GROUP BY maquina ORDER BY maquina;";

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_double(st, 1, umbral);

    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        snprintf(out[n].maquina, sizeof out[n].maquina, "%s",
                 (const char *)sqlite3_column_text(st, 0));
        out[n].validas      = sqlite3_column_int64(st, 1);
        out[n].descartadas  = sqlite3_column_int64(st, 2);
        out[n].temp_min     = sqlite3_column_double(st, 3);
        out[n].temp_max     = sqlite3_column_double(st, 4);
        out[n].temp_prom    = sqlite3_column_double(st, 5);
        out[n].sobre_umbral = sqlite3_column_int64(st, 6);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

/* ================= tabla telemetria ================= */

/* Guarda una lectura nueva con el siguiente numero de secuencia y enviado = 0.
   El seq se calcula con MAX(seq)+1, asi que sobrevive a un reinicio del programa.
   Devuelve el seq guardado, o -1 si falla. */
long long datos_guardar_telemetria(sqlite3 *db, double temp, double corriente) {
    long long seq = datos_valor_entero(db, "SELECT COALESCE(MAX(seq), 0) + 1 FROM telemetria;");
    if (seq < 1) return -1;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO telemetria (seq, ts, temp, corriente, enviado, intentos) "
            "VALUES (?, ?, ?, ?, 0, 0);", -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_int64(st, 1, seq);
    sqlite3_bind_int64(st, 2, (long long)time(NULL));   /* hora en que se creo la lectura */
    sqlite3_bind_double(st, 3, temp);
    sqlite3_bind_double(st, 4, corriente);

    int paso = sqlite3_step(st);
    if (paso != SQLITE_DONE)
        fprintf(stderr, "Error guardando lectura: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return paso == SQLITE_DONE ? seq : -1;
}

/* Busca la lectura pendiente mas antigua (menor seq). Devuelve 1 si hay, 0 si no. */
int datos_siguiente_pendiente(sqlite3 *db, struct Lectura *l) {
    sqlite3_stmt *st = NULL;
    int hay = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT seq, ts, temp, corriente FROM telemetria "
            "WHERE enviado = 0 ORDER BY seq LIMIT 1;", -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        l->seq       = sqlite3_column_int64(st, 0);
        l->ts        = sqlite3_column_int64(st, 1);
        l->temp      = sqlite3_column_double(st, 2);
        l->corriente = sqlite3_column_double(st, 3);
        hay = 1;
    }
    sqlite3_finalize(st);
    return hay;
}

/* Anota un intento: suma 1 a intentos y, si llego al servidor, enviado = 1 */
void datos_anotar_intento(sqlite3 *db, long long seq, int entregada) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "UPDATE telemetria SET intentos = intentos + 1, enviado = ? WHERE seq = ?;",
            -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare: %s\n", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int(st, 1, entregada ? 1 : 0);
    sqlite3_bind_int64(st, 2, seq);
    if (sqlite3_step(st) != SQLITE_DONE)
        fprintf(stderr, "Error anotando intento: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(st);
}

long long datos_contar_pendientes(sqlite3 *db) {
    return datos_valor_entero(db, "SELECT COUNT(*) FROM telemetria WHERE enviado = 0;");
}

long long datos_ultimo_seq(sqlite3 *db) {
    return datos_valor_entero(db, "SELECT COALESCE(MAX(seq), 0) FROM telemetria;");
}

/* Borra las lecturas que no se enviaron. Devuelve cuantas borro, o -1 si falla. */
int datos_borrar_pendientes(sqlite3 *db) {
    if (sqlite3_exec(db, "DELETE FROM telemetria WHERE enviado = 0;",
                     NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    return sqlite3_changes(db);
}
