/* sensor.c - Lectura del sensor (simulada) */
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include "sensor.h"

void sensor_iniciar(void) {
    srand((unsigned)time(NULL));
}

void sensor_leer(double *temp, double *corriente) {
    static long n = 0;                                    /* cuenta de lecturas: mueve la onda */
    double ruido1 = ((double)rand() / RAND_MAX) - 0.5;    /* -0.5 a 0.5 */
    double ruido2 = ((double)rand() / RAND_MAX) - 0.5;
    *temp      = 35.0 + 8.0 * sin(n / 20.0) + 2.0 * ruido1;     /* 26 .. 44 C */
    *corriente = 3.5 + 1.2 * sin(n / 13.0) + 0.6 * ruido2;      /* 2.0 .. 5.0 A */
    n++;
}
