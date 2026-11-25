/* sensor.h - Lectura del sensor (simulada) */
#ifndef SENSOR_H
#define SENSOR_H

/* Inicializa el generador de ruido. Llamar una vez antes de leer. */
void sensor_iniciar(void);

/* Una lectura simulada: temperatura 25-45 C (onda lenta + ruido) y corriente 2-5 A.
   Para un DHT22 real, solo hay que cambiar el cuerpo de esta funcion. */
void sensor_leer(double *temp, double *corriente);

#endif
