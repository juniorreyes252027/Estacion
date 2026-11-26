/* red.h - Todo lo que usa libcurl (HTTP ahora, MQTT en los Niveles 4 y 5) */
#ifndef RED_H
#define RED_H

#include <stddef.h>
#include <curl/curl.h>
#include "datos.h"      /* para struct Lectura */

#define USER_AGENT "estacion-itla/1.0"

/* Cuerpo de una respuesta HTTP. Liberar .datos con free(). */
struct Buffer {
    char  *datos;
    size_t largo;
};

/* Que paso al intentar enviar una lectura */
enum Envio {
    ENVIO_OK,          /* el servidor la tiene (201 creada o 200 duplicada)     */
    ENVIO_ESPERAR,     /* 503 o 429: hay que esperar antes de volver a intentar */
    ENVIO_RED,         /* no hubo respuesta: red caida, timeout...              */
    ENVIO_RECHAZADA    /* 400, 401...: el servidor no la acepta                 */
};

/* Handle de libcurl ya configurado (token, User-Agent, tiempos).
   La lista de cabeceras queda en *cabeceras: liberarla con curl_slist_free_all. */
CURL *red_nuevo_cliente(const char *token, struct curl_slist **cabeceras);

/* GET si json es NULL, POST si lleva JSON.
   Devuelve 0 si el servidor respondio (codigo HTTP en *codigo) o -1 si fallo la red.
   El cuerpo queda en resp->datos (liberar con free).
   Si retry_after no es NULL, ahi queda la cabecera Retry-After (0 si no vino). */
int red_peticion(CURL *curl, const char *url, const char *json,
                 long *codigo, struct Buffer *resp, long *retry_after);

/* POST /api/v1/telemetria con una lectura.
   *espera: segundos que pide esperar el servidor (si devuelve ENVIO_ESPERAR).
   *codigo: codigo HTTP recibido (0 si no hubo respuesta). */
enum Envio red_enviar_lectura(CURL *curl, const char *host, const struct Lectura *l,
                              long *espera, long *codigo);

/* ---- MQTT (Nivel 4: latido) ---- */

/* Handle de libcurl para MQTT. El usuario es la matricula y la contrasena es el token. */
CURL *red_nuevo_mqtt(const char *matricula, const char *token);

/* Publica LATIDO;<seq>;<pendientes>;<ts> en itla/estudiantes/<MATRICULA>/latido (QoS 0).
   Devuelve 1 si se publico, 0 si no. No se reintenta ni se encola: un latido perdido se pierde. */
int red_publicar_latido(CURL *mqtt, const char *host, const char *matricula,
                        long long seq, long long pendientes);

/* Duerme ms milisegundos (funciona igual en Windows y Linux). */
void red_pausa_ms(int ms);

#endif
