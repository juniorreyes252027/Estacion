/* comandos.c - Los subcomandos: usan datos.c (SQLite), red.c (libcurl) y sensor.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include "comandos.h"
#include "datos.h"
#include "red.h"
#include "sensor.h"

#define LIMITE_PAGINA 200
#define MAX_MAQUINAS  16

/* El host y el token vienen de variables de entorno, nunca del codigo */
static int leer_entorno(const char **host, const char **token) {
    *host  = getenv("ITLA_HOST");
    *token = getenv("ITLA_TOKEN");
    if (!*host || !*token) {
        fprintf(stderr, "Faltan las variables ITLA_HOST o ITLA_TOKEN\n");
        return -1;
    }
    return 0;
}

/* ================= Nivel 0 ================= */

/* Pide la configuracion, la guarda en la tabla config e imprime lo principal */
int cmd_config(void) {
    const char *host, *token;
    if (leer_entorno(&host, &token) != 0) return 1;

    struct curl_slist *cabeceras = NULL;
    CURL *curl = red_nuevo_cliente(token, &cabeceras);
    if (!curl) return 1;

    char url[256];
    snprintf(url, sizeof url, "http://%s:4000/api/v1/config", host);

    int ok = 0;
    long codigo = 0;
    struct Buffer buf = { NULL, 0 };

    if (red_peticion(curl, url, NULL, &codigo, &buf, NULL) == 0) {
        if (codigo != 200) {
            fprintf(stderr, "El servidor respondio HTTP %ld\n", codigo);
        } else if (buf.datos) {
            sqlite3 *db = datos_abrir();
            if (db) {
                if (datos_guardar_config(db, buf.datos) == 0) {
                    char umbral[64] = "?", intervalo[64] = "?", hora[64] = "?";
                    datos_leer_config(db, "umbral_temp", umbral, sizeof umbral);
                    datos_leer_config(db, "intervalo_s", intervalo, sizeof intervalo);
                    datos_leer_config(db, "hora", hora, sizeof hora);
                    printf("umbral_temp      = %s\n", umbral);
                    printf("intervalo_s      = %s\n", intervalo);
                    printf("hora del servidor = %s\n", hora);
                    ok = 1;
                }
                datos_cerrar(db);
            }
        }
    }

    free(buf.datos);
    curl_slist_free_all(cabeceras);
    curl_easy_cleanup(curl);
    return ok ? 0 : 1;
}

/* ================= Nivel 1 ================= */

/* Descarga todas las lecturas por paginas, las guarda en SQLite
   y envia la respuesta al servidor */
int cmd_descargar(void) {
    const char *host, *token;
    if (leer_entorno(&host, &token) != 0) return 1;

    /* Todo se declara arriba para poder limpiar al final en un solo lugar */
    int ok = 0;
    sqlite3 *db = NULL;
    sqlite3_stmt *ins = NULL;
    CURL *curl = NULL;
    struct curl_slist *cabeceras = NULL;
    int en_transaccion = 0;

    db = datos_abrir();
    if (!db) return 1;

    curl = red_nuevo_cliente(token, &cabeceras);
    if (!curl) goto limpiar;

    ins = datos_preparar_planta(db);
    if (!ins) goto limpiar;

    if (datos_begin(db) != 0) goto limpiar;
    en_transaccion = 1;

    long long desde_id = 0;
    int paginas = 0;

    for (;;) {
        char url[256];
        snprintf(url, sizeof url,
                 "http://%s:4000/api/v1/lecturas?desde_id=%lld&limite=%d",
                 host, desde_id, LIMITE_PAGINA);

        struct Buffer buf = { NULL, 0 };
        long codigo = 0;
        if (red_peticion(curl, url, NULL, &codigo, &buf, NULL) != 0) {
            free(buf.datos);
            goto limpiar;
        }
        if (codigo != 200) {
            fprintf(stderr, "El servidor respondio HTTP %ld\n", codigo);
            free(buf.datos);
            goto limpiar;
        }

        long long max_id = desde_id;
        int filas = buf.datos ? datos_guardar_pagina(db, ins, buf.datos, &max_id) : 0;
        free(buf.datos);
        if (filas < 0) goto limpiar;

        paginas++;
        printf("Pagina %d: %d filas (id mas alto: %lld)\n", paginas, filas, max_id);

        if (filas < LIMITE_PAGINA) break;              /* ultima pagina */
        if (max_id <= desde_id) {
            fprintf(stderr, "El id no avanza; se detiene para no repetir para siempre.\n");
            goto limpiar;
        }
        desde_id = max_id;
    }

    if (datos_commit(db) != 0) goto limpiar;
    en_transaccion = 0;
    ok = 1;

    long long filas_unicas = datos_filas_unicas(db);
    long long suma_ids     = datos_suma_ids(db);
    printf("\nfilas_unicas = %lld\n", filas_unicas);
    printf("suma_ids     = %lld\n", suma_ids);

    /* Armar el JSON y enviarlo con POST */
    char json[128];
    snprintf(json, sizeof json, "{\"filas_unicas\":%lld,\"suma_ids\":%lld}",
             filas_unicas, suma_ids);
    char url_resp[256];
    snprintf(url_resp, sizeof url_resp,
             "http://%s:4000/api/v1/respuestas/descarga", host);

    struct Buffer resp = { NULL, 0 };
    long cod_post = 0;
    if (red_peticion(curl, url_resp, json, &cod_post, &resp, NULL) != 0) {
        ok = 0;
    } else {
        printf("\nRespuesta del servidor (HTTP %ld):\n%s\n",
               cod_post, resp.datos ? resp.datos : "");
        if (cod_post != 200) ok = 0;
    }
    free(resp.datos);

limpiar:
    if (en_transaccion) datos_rollback(db);
    datos_finalizar(ins);
    datos_cerrar(db);
    curl_slist_free_all(cabeceras);
    if (curl) curl_easy_cleanup(curl);
    return ok ? 0 : 1;
}

/* ================= Nivel 2 ================= */

/* Estadisticas por maquina (calculadas con SQL en datos.c).
   Sin "enviar" solo muestra la tabla; con "enviar" tambien la manda al servidor. */
int cmd_analizar(int enviar) {
    sqlite3 *db = datos_abrir();
    if (!db) return 1;

    char texto[64];
    if (!datos_leer_config(db, "umbral_temp", texto, sizeof texto)) {
        fprintf(stderr, "No hay umbral_temp en la base. Ejecuta antes: estacion config\n");
        datos_cerrar(db);
        return 1;
    }
    double umbral = strtod(texto, NULL);

    struct Estadistica est[MAX_MAQUINAS];
    int n = datos_estadisticas(db, umbral, est, MAX_MAQUINAS);
    datos_cerrar(db);
    if (n < 0) return 1;
    if (n == 0) {
        fprintf(stderr, "No hay lecturas. Ejecuta antes: estacion descargar\n");
        return 1;
    }

    char json[2048];
    size_t pos = 0;
    pos += (size_t)snprintf(json + pos, sizeof json - pos, "{\"maquinas\":[");

    printf("umbral_temp = %.2f\n\n", umbral);
    printf("%-8s %7s %11s %9s %9s %9s %13s\n",
           "maquina", "validas", "descartadas", "temp_min", "temp_max",
           "temp_prom", "sobre_umbral");

    for (int i = 0; i < n; i++) {
        const struct Estadistica *e = &est[i];
        printf("%-8s %7lld %11lld %9.2f %9.2f %9.2f %13lld\n",
               e->maquina, e->validas, e->descartadas,
               e->temp_min, e->temp_max, e->temp_prom, e->sobre_umbral);

        if (pos < sizeof json)
            pos += (size_t)snprintf(json + pos, sizeof json - pos,
                "%s{\"maquina\":\"%s\",\"validas\":%lld,\"descartadas\":%lld,"
                "\"temp_min\":%.2f,\"temp_max\":%.2f,\"temp_prom\":%.2f,"
                "\"sobre_umbral\":%lld}",
                i ? "," : "", e->maquina, e->validas, e->descartadas,
                e->temp_min, e->temp_max, e->temp_prom, e->sobre_umbral);
    }
    if (pos < sizeof json) snprintf(json + pos, sizeof json - pos, "]}");

    if (!enviar) {
        printf("\nSolo se mostraron los resultados. Para enviarlos: estacion analizar enviar\n");
        return 0;
    }

    const char *host, *token;
    if (leer_entorno(&host, &token) != 0) return 1;

    struct curl_slist *cabeceras = NULL;
    CURL *curl = red_nuevo_cliente(token, &cabeceras);
    if (!curl) return 1;

    char url[256];
    snprintf(url, sizeof url, "http://%s:4000/api/v1/respuestas/analisis", host);

    struct Buffer resp = { NULL, 0 };
    long codigo = 0;
    int ok = 0;
    if (red_peticion(curl, url, json, &codigo, &resp, NULL) == 0) {
        printf("\nRespuesta del servidor (HTTP %ld):\n%s\n",
               codigo, resp.datos ? resp.datos : "");
        ok = (codigo == 200);
    }
    free(resp.datos);
    curl_slist_free_all(cabeceras);
    curl_easy_cleanup(curl);
    return ok ? 0 : 1;
}

/* ================= Nivel 3: pruebas y limpieza ================= */

/* Prueba, paso A: guarda 3 lecturas sin tocar la red */
int cmd_lectura(void) {
    sqlite3 *db = datos_abrir();
    if (!db) return 1;
    sensor_iniciar();

    for (int i = 0; i < 3; i++) {
        double temp, corriente;
        sensor_leer(&temp, &corriente);
        long long seq = datos_guardar_telemetria(db, temp, corriente);
        if (seq < 0) { datos_cerrar(db); return 1; }
        printf("Lectura guardada con seq = %lld\n", seq);
    }
    printf("Pendientes de enviar: %lld\n", datos_contar_pendientes(db));
    datos_cerrar(db);
    return 0;
}

/* Prueba, paso B: envia UNA lectura pendiente y cuenta que paso */
int cmd_enviar_una(void) {
    const char *host, *token;
    if (leer_entorno(&host, &token) != 0) return 1;

    sqlite3 *db = datos_abrir();
    if (!db) return 1;

    struct Lectura l;
    if (!datos_siguiente_pendiente(db, &l)) {
        printf("No hay lecturas pendientes. Genera algunas con: estacion lectura\n");
        datos_cerrar(db);
        return 0;
    }

    struct curl_slist *cabeceras = NULL;
    CURL *curl = red_nuevo_cliente(token, &cabeceras);
    if (!curl) {
        datos_cerrar(db);
        return 1;
    }

    printf("Enviando seq=%lld ts=%lld temp=%.2f corriente=%.2f\n",
           l.seq, l.ts, l.temp, l.corriente);

    long espera = 0, codigo = 0;
    enum Envio r = red_enviar_lectura(curl, host, &l, &espera, &codigo);

    switch (r) {
    case ENVIO_OK:
        datos_anotar_intento(db, l.seq, 1);
        printf("HTTP %ld: entregada. La seq %lld queda marcada como enviada.\n", codigo, l.seq);
        break;
    case ENVIO_ESPERAR:
        datos_anotar_intento(db, l.seq, 0);
        printf("HTTP %ld: el servidor pide esperar %ld s. La lectura sigue pendiente.\n",
               codigo, espera);
        break;
    case ENVIO_RED:
        datos_anotar_intento(db, l.seq, 0);
        printf("Sin respuesta del servidor. La lectura sigue pendiente.\n");
        break;
    case ENVIO_RECHAZADA:
        datos_anotar_intento(db, l.seq, 0);
        printf("El servidor la rechazo (mira el mensaje de arriba).\n");
        break;
    }

    curl_slist_free_all(cabeceras);
    curl_easy_cleanup(curl);
    datos_cerrar(db);
    return r == ENVIO_OK ? 0 : 1;
}

/* Borra las lecturas que no se enviaron (por ejemplo las de prueba) */
int cmd_limpiar(void) {
    sqlite3 *db = datos_abrir();
    if (!db) return 1;
    int borradas = datos_borrar_pendientes(db);
    datos_cerrar(db);
    if (borradas < 0) return 1;
    printf("Lecturas pendientes borradas: %d\n", borradas);
    return 0;
}

/* ================= Nivel 3: bucle de telemetria ================= */

static volatile sig_atomic_t terminar = 0;

/* Se llama cuando el usuario presiona Ctrl+C: el bucle termina con calma */
static void al_interrumpir(int senal) {
    (void)senal;
    terminar = 1;
}

/* Escribe la hora local como HH:MM:SS */
static void hora_actual(char *salida, size_t n) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    if (tm) strftime(salida, n, "%H:%M:%S", tm);
    else    snprintf(salida, n, "--:--:--");
}

/* Nivel 3 y 4: cada intervalo_s genera una lectura y la envia sin perder ninguna;
   cada latido_s publica un latido MQTT.
   Las lecturas que no se pueden enviar esperan en la tabla telemetria y se
   mandan despues, en orden de seq. Termina con Ctrl+C. */
int cmd_enviar(void) {
    const char *host, *token;
    if (leer_entorno(&host, &token) != 0) return 1;

    sqlite3 *db = datos_abrir();
    if (!db) return 1;

    struct curl_slist *cabeceras = NULL;
    CURL *curl = red_nuevo_cliente(token, &cabeceras);    /* un solo handle para todo el bucle */
    if (!curl) {
        datos_cerrar(db);
        return 1;
    }

    long intervalo = 5;
    char texto[64];
    if (datos_leer_config(db, "intervalo_s", texto, sizeof texto)) {
        long v = strtol(texto, NULL, 10);
        if (v >= 1) intervalo = v;
    } else {
        printf("(No hay intervalo_s en la base; uso 5 s. Ejecuta antes: estacion config)\n");
    }

    long latido_s = 10;                    /* cada cuantos segundos va un latido MQTT */
    if (datos_leer_config(db, "latido_s", texto, sizeof texto)) {
        long v = strtol(texto, NULL, 10);
        if (v >= 1) latido_s = v;
    }

    /* Nivel 4: el latido necesita la matricula (usuario MQTT y parte del topico) */
    const char *matricula = getenv("ITLA_MATRICULA");
    CURL *mqtt = NULL;
    if (matricula) {
        mqtt = red_nuevo_mqtt(matricula, token);
    } else {
        printf("(Falta ITLA_MATRICULA: no se enviaran latidos MQTT)\n");
    }

    sensor_iniciar();
    signal(SIGINT, al_interrumpir);

    time_t proximo_latido = time(NULL);    /* el primer latido sale enseguida */
    long latidos = 0;
    time_t proxima_lectura = time(NULL);   /* la primera lectura se genera enseguida */
    time_t no_enviar_antes = 0;            /* mientras no llegue esta hora, no se envia */
    int fallos_red = 0;                    /* fallos de red seguidos (para esperar 1, 2, 4... s) */
    long enviadas = 0;
    int abortar = 0;
    char hora[16];

    printf("Estacion enviando una lectura cada %ld s. Ctrl+C para terminar.\n", intervalo);

    while (!terminar && !abortar) {
        time_t ahora = time(NULL);

        /* 1. Si toca, generar una lectura nueva y guardarla en la cola */
        if (ahora >= proxima_lectura) {
            double temp, corriente;
            sensor_leer(&temp, &corriente);
            if (datos_guardar_telemetria(db, temp, corriente) < 0) {
                abortar = 1;
                break;
            }
            proxima_lectura += intervalo;
            if (proxima_lectura <= ahora) proxima_lectura = ahora + intervalo;
        }

        /* 2. Si no estamos esperando, enviar UNA lectura: la pendiente mas antigua */
        if (ahora >= no_enviar_antes) {
            struct Lectura l;
            if (datos_siguiente_pendiente(db, &l)) {
                long espera = 0, codigo = 0;
                enum Envio r = red_enviar_lectura(curl, host, &l, &espera, &codigo);
                hora_actual(hora, sizeof hora);

                switch (r) {
                case ENVIO_OK:
                    datos_anotar_intento(db, l.seq, 1);
                    enviadas++;
                    fallos_red = 0;
                    printf("[%s] seq %lld enviada (HTTP %ld). Pendientes: %lld\n",
                           hora, l.seq, codigo, datos_contar_pendientes(db));
                    break;
                case ENVIO_ESPERAR:
                    datos_anotar_intento(db, l.seq, 0);
                    no_enviar_antes = time(NULL) + espera + 1;   /* +1 s de margen */
                    printf("[%s] HTTP %ld: servidor ocupado. Espero %ld s. Pendientes: %lld\n",
                           hora, codigo, espera, datos_contar_pendientes(db));
                    break;
                case ENVIO_RED:
                    datos_anotar_intento(db, l.seq, 0);
                    fallos_red++;
                    espera = 1L << (fallos_red > 5 ? 5 : fallos_red - 1);   /* 1,2,4,8,16,32 */
                    if (espera > 30) espera = 30;
                    no_enviar_antes = time(NULL) + espera;
                    printf("[%s] Sin conexion. Reintento en %ld s. Pendientes: %lld\n",
                           hora, espera, datos_contar_pendientes(db));
                    break;
                case ENVIO_RECHAZADA:
                    datos_anotar_intento(db, l.seq, 0);
                    printf("[%s] El servidor rechaza las lecturas (HTTP %ld). Me detengo.\n",
                           hora, codigo);
                    abortar = 1;
                    break;
                }
            }
        }

        /* 3. Si toca, publicar el latido MQTT. Va por su cuenta: aunque el HTTP este
              esperando por un 503, el latido sale a su hora. */
        if (mqtt && time(NULL) >= proximo_latido) {
            if (red_publicar_latido(mqtt, host, matricula,
                                    datos_ultimo_seq(db), datos_contar_pendientes(db))) {
                latidos++;
                hora_actual(hora, sizeof hora);
                printf("[%s] latido #%ld publicado\n", hora, latidos);
            }
            time_t ahora2 = time(NULL);
            proximo_latido += latido_s;                     /* sin acumular retraso */
            if (proximo_latido <= ahora2) proximo_latido = ahora2 + latido_s;
        }

        red_pausa_ms(250);  /* 4 vueltas por segundo: nunca pasa de 5 peticiones/s */
    }

    printf("\nTerminando. Lecturas enviadas: %ld. Latidos publicados: %ld. Pendientes en la cola: %lld\n",
           enviadas, latidos, datos_contar_pendientes(db));

    if (mqtt) curl_easy_cleanup(mqtt);
    curl_slist_free_all(cabeceras);
    curl_easy_cleanup(curl);
    datos_cerrar(db);
    return abortar ? 1 : 0;
}
