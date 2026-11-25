/* datos.h - Todo lo que toca SQLite (estacion.db) */
#ifndef DATOS_H
#define DATOS_H

#include <stddef.h>
#include "sqlite3.h"

#define DB_FILE "estacion.db"

/* Una fila de la tabla telemetria */
struct Lectura {
    long long seq;
    long long ts;
    double    temp;
    double    corriente;
};

/* Resultado del analisis de una maquina (Nivel 2) */
struct Estadistica {
    char      maquina[16];
    long long validas;
    long long descartadas;
    double    temp_min;
    double    temp_max;
    double    temp_prom;
    long long sobre_umbral;
};

/* ---- conexion ---- */
sqlite3 *datos_abrir(void);                 /* abre estacion.db y crea las tablas */
void     datos_cerrar(sqlite3 *db);
void     datos_finalizar(sqlite3_stmt *st); /* acepta NULL */
long long datos_valor_entero(sqlite3 *db, const char *sql);

/* ---- transacciones ---- */
int  datos_begin(sqlite3 *db);              /* 0 = bien */
int  datos_commit(sqlite3 *db);
void datos_rollback(sqlite3 *db);

/* ---- tabla config ---- */
int datos_guardar_config(sqlite3 *db, char *texto);   /* texto: lineas clave=valor */
int datos_leer_config(sqlite3 *db, const char *clave, char *salida, size_t n);

/* ---- tabla lecturas_planta (Nivel 1 y 2) ---- */
sqlite3_stmt *datos_preparar_planta(sqlite3 *db);     /* INSERT OR IGNORE preparado */
int datos_guardar_pagina(sqlite3 *db, sqlite3_stmt *ins, char *csv, long long *max_id);
long long datos_filas_unicas(sqlite3 *db);
long long datos_suma_ids(sqlite3 *db);
int datos_estadisticas(sqlite3 *db, double umbral, struct Estadistica *out, int max);

/* ---- tabla telemetria (Nivel 3) ---- */
long long datos_guardar_telemetria(sqlite3 *db, double temp, double corriente);
int       datos_siguiente_pendiente(sqlite3 *db, struct Lectura *l);
void      datos_anotar_intento(sqlite3 *db, long long seq, int entregada);
long long datos_contar_pendientes(sqlite3 *db);
long long datos_ultimo_seq(sqlite3 *db);
int       datos_borrar_pendientes(sqlite3 *db);       /* devuelve cuantas borro, -1 si falla */

#endif
