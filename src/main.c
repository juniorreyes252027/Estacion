/* main.c - Punto de entrada: lee argv y llama al subcomando */
#include <stdio.h>
#include <string.h>
#include <curl/curl.h>
#include "comandos.h"

static void uso(void) {
    fprintf(stderr,
        "Uso: estacion <subcomando>\n"
        "  config            descarga y guarda la configuracion (Nivel 0)\n"
        "  descargar         descarga las lecturas de la planta (Nivel 1)\n"
        "  analizar [enviar] estadisticas por maquina (Nivel 2)\n"
        "  lectura           guarda 3 lecturas de prueba\n"
        "  enviar-una        envia una lectura pendiente (prueba)\n"
        "  limpiar           borra las lecturas pendientes\n"
        "  enviar            telemetria continua + latido (Niveles 3 y 4)\n"
        "  escuchar          recibe y confirma alarmas (Nivel 5)\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        uso();
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    int resultado = 1;
    const char *sub = argv[1];

    if (strcmp(sub, "config") == 0)
        resultado = cmd_config();
    else if (strcmp(sub, "descargar") == 0)
        resultado = cmd_descargar();
    else if (strcmp(sub, "analizar") == 0)
        resultado = cmd_analizar(argc > 2 && strcmp(argv[2], "enviar") == 0);
    else if (strcmp(sub, "lectura") == 0)
        resultado = cmd_lectura();
    else if (strcmp(sub, "enviar-una") == 0)
        resultado = cmd_enviar_una();
    else if (strcmp(sub, "limpiar") == 0)
        resultado = cmd_limpiar();
    else if (strcmp(sub, "enviar") == 0)
        resultado = cmd_enviar();
    else if (strcmp(sub, "escuchar") == 0)
        resultado = cmd_escuchar();
    else {
        fprintf(stderr, "Subcomando desconocido: %s\n\n", sub);
        uso();
    }

    curl_global_cleanup();
    return resultado;
}
