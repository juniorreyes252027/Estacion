/* red.c - Todo lo que usa libcurl */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "red.h"

/* libcurl llama a esta funcion cada vez que llega un trozo de respuesta */
static size_t al_recibir(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t n = size * nmemb;
    struct Buffer *buf = (struct Buffer *)userdata;

    char *nuevo = realloc(buf->datos, buf->largo + n + 1);
    if (!nuevo) return 0;
    buf->datos = nuevo;
    memcpy(buf->datos + buf->largo, ptr, n);
    buf->largo += n;
    buf->datos[buf->largo] = '\0';
    return n;
}

/* Devuelve 1 si el texto s empieza con prefijo (sin distinguir mayusculas) */
static int empieza_con(const char *s, const char *prefijo) {
    for (; *prefijo; s++, prefijo++) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != *prefijo) return 0;
    }
    return 1;
}

/* libcurl llama a esta funcion por cada linea de cabecera de la respuesta.
   Si es "Retry-After: N", guarda N en la variable apuntada por userdata. */
static size_t al_recibir_cabecera(char *ptr, size_t size, size_t nitems, void *userdata) {
    size_t n = size * nitems;
    long *retry_after = (long *)userdata;
    if (!retry_after) return n;

    char linea[64];                              /* copia con '\0' al final */
    size_t m = n < sizeof linea - 1 ? n : sizeof linea - 1;
    memcpy(linea, ptr, m);
    linea[m] = '\0';

    if (empieza_con(linea, "retry-after:"))
        *retry_after = strtol(linea + 12, NULL, 10);
    return n;
}

CURL *red_nuevo_cliente(const char *token, struct curl_slist **cabeceras) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    char auth[256];
    snprintf(auth, sizeof auth, "Authorization: Bearer %s", token);
    *cabeceras = curl_slist_append(NULL, auth);
    *cabeceras = curl_slist_append(*cabeceras, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, *cabeceras);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, al_recibir);
    return curl;
}

int red_peticion(CURL *curl, const char *url, const char *json,
                 long *codigo, struct Buffer *resp, long *retry_after) {
    resp->datos = NULL;
    resp->largo = 0;
    if (retry_after) *retry_after = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, al_recibir_cabecera);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, retry_after);
    if (json) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    else      curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {                         /* nivel 1 de error: red */
        fprintf(stderr, "Error de red: %s\n", curl_easy_strerror(res));
        return -1;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, codigo);   /* nivel 2: HTTP */
    return 0;
}

enum Envio red_enviar_lectura(CURL *curl, const char *host, const struct Lectura *l,
                              long *espera, long *codigo) {
    char url[256];
    snprintf(url, sizeof url, "http://%s:4000/api/v1/telemetria", host);

    char json[160];
    snprintf(json, sizeof json,
             "{\"seq\":%lld,\"ts\":%lld,\"temp\":%.2f,\"corriente\":%.2f}",
             l->seq, l->ts, l->temp, l->corriente);

    struct Buffer resp = { NULL, 0 };
    long retry = 0;
    enum Envio resultado;

    *codigo = 0;
    if (red_peticion(curl, url, json, codigo, &resp, &retry) != 0) {
        resultado = ENVIO_RED;
    } else if (*codigo == 201 || *codigo == 200) {
        resultado = ENVIO_OK;                    /* 200 = duplicada: tambien esta guardada */
    } else if (*codigo == 503 || *codigo == 429) {
        *espera = retry > 0 ? retry : 5;         /* si no dice cuanto, esperamos 5 s */
        resultado = ENVIO_ESPERAR;
    } else {
        fprintf(stderr, "El servidor rechazo la lectura %lld (HTTP %ld): %s\n",
                l->seq, *codigo, resp.datos ? resp.datos : "");
        resultado = ENVIO_RECHAZADA;
    }
    free(resp.datos);
    return resultado;
}

/* ================= MQTT ================= */

/* Un publicador no espera datos de vuelta: lo que llegue se descarta */
static size_t descartar(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

CURL *red_nuevo_mqtt(const char *matricula, const char *token) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    curl_easy_setopt(curl, CURLOPT_USERNAME, matricula);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, token);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);          /* corto: el bucle no debe quedarse esperando */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, descartar);
    return curl;
}

int red_publicar_latido(CURL *mqtt, const char *host, const char *matricula,
                        long long seq, long long pendientes) {
    char url[256];
    snprintf(url, sizeof url, "mqtt://%s:1883/itla/estudiantes/%s/latido", host, matricula);

    char mensaje[100];
    snprintf(mensaje, sizeof mensaje, "LATIDO;%lld;%lld;%lld",
             seq, pendientes, (long long)time(NULL));

    curl_easy_setopt(mqtt, CURLOPT_URL, url);
    curl_easy_setopt(mqtt, CURLOPT_POSTFIELDS, mensaje);   /* con POSTFIELDS, libcurl PUBLICA */

    CURLcode res = curl_easy_perform(mqtt);
    if (res != CURLE_OK) {
        fprintf(stderr, "Latido no publicado: %s\n", curl_easy_strerror(res));
        return 0;
    }
    return 1;
}

void red_pausa_ms(int ms) {
    CURLM *m = curl_multi_init();
    if (!m) return;
    curl_multi_poll(m, NULL, 0, ms, NULL);
    curl_multi_cleanup(m);
}
