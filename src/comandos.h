/* comandos.h - Un subcomando de la linea de comandos = una funcion */
#ifndef COMANDOS_H
#define COMANDOS_H

/* Todas devuelven 0 si salio bien y 1 si fallo (es el codigo de salida del programa). */
int cmd_config(void);              /* Nivel 0 */
int cmd_descargar(void);           /* Nivel 1 */
int cmd_analizar(int enviar);      /* Nivel 2 */
int cmd_lectura(void);             /* Nivel 3, prueba: guarda 3 lecturas */
int cmd_enviar_una(void);          /* Nivel 3, prueba: envia 1 lectura */
int cmd_limpiar(void);             /* borra las lecturas pendientes */
int cmd_enviar(void);              /* Nivel 3: bucle de telemetria */

#endif
