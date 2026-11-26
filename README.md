# Estación de Telemetría ITLA

Programa en C (C11) que se conecta al servidor del reto con **libcurl**, guarda
todo en **SQLite** y envía telemetría de forma resistente a fallos.

## Estructura

| Archivo | Qué hace |
|---|---|
| `src/main.c` | Lee `argv` y llama al subcomando |
| `src/comandos.c/.h` | Un subcomando = una función |
| `src/red.c/.h` | Todo lo de libcurl (HTTP y MQTT) |
| `src/datos.c/.h` | Todo lo de SQLite (`estacion.db`) |
| `src/sensor.c/.h` | Sensor simulado (temp 25–45 °C, corriente 2–5 A) |
| `lib/sqlite/` | SQLite amalgamation (`sqlite3.c`, `sqlite3.h`) |
| `Makefile` | Compilación (`all`, `run`, `clean`) |

## Requisitos

- `gcc` y `make`
- libcurl del sistema
  - Raspberry Pi OS / Ubuntu: `sudo apt install build-essential libcurl4-openssl-dev`
  - Windows (MSYS2 UCRT64): `pacman -S make mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-curl`
- `lib/sqlite/sqlite3.c` y `sqlite3.h` (amalgamation descargada de sqlite.org)

## Compilar

```
make            # deja el programa en build/estacion
make run        # compila y ejecuta "config"
make run ARGS=analizar
make clean      # borra build/ (no toca estacion.db)
```

SQLite se compila una sola vez a `build/sqlite3.o`; después `make` solo recompila lo que cambió.
Se usa `-std=c11 -Wall -Wextra` y no debe dar ninguna advertencia.

## Credenciales

El token **nunca** se escribe en el código ni en el repositorio: se lee de variables de entorno.

Linux / MSYS2:
```
export ITLA_HOST=<direccion-del-servidor>
export ITLA_TOKEN=<tu-token>
export ITLA_MATRICULA=<tu-matricula>
```
PowerShell:
```
$env:ITLA_HOST="<direccion-del-servidor>"
$env:ITLA_TOKEN="<tu-token>"
$env:ITLA_MATRICULA="<tu-matricula>"
```

## Uso

```
./build/estacion config            # Nivel 0: guarda la configuración del servidor
./build/estacion descargar         # Nivel 1: descarga las lecturas y responde
./build/estacion analizar          # Nivel 2: estadísticas por máquina (solo muestra)
./build/estacion analizar enviar   # Nivel 2: las muestra y las envía
./build/estacion enviar            # Nivel 3 y 4: telemetría + latido MQTT cada latido_s (Ctrl+C para parar)
./build/estacion escuchar          # Nivel 5: recibe, guarda y confirma alarmas (Ctrl+C para parar)
./build/estacion lectura           # prueba: guarda 3 lecturas
./build/estacion enviar-una        # prueba: envía 1 lectura pendiente
./build/estacion limpiar           # borra las lecturas pendientes
```

## Latido MQTT (Nivel 4)

Mientras `enviar` corre, cada `latido_s` segundos (10 por defecto, viene de `/config`) publica
`LATIDO;<seq>;<pendientes>;<ts>` en `itla/estudiantes/<MATRICULA>/latido` con QoS 0.
Usa libcurl con una URL `mqtt://` (usuario = matrícula, contraseña = token). El latido tiene su
propio reloj: sale a su hora aunque el HTTP esté esperando por un 503, y si se pierde no se reenvía.
Requiere la variable `ITLA_MATRICULA`.

## Alarmas (Nivel 5)

Se usan **dos terminales** en la misma carpeta (comparten `estacion.db`), ambas con las variables de entorno:

```
./build/estacion enviar      # terminal 1: telemetría y latido
./build/estacion escuchar    # terminal 2: alarmas
```

El servidor solo manda alarmas si hubo un latido en los últimos 60 s. Requiere `ITLA_MATRICULA`.

1. `escuchar` se suscribe a `itla/planta/<MATRICULA>/alarma` (MQTT con libcurl).
2. Cada mensaje `ALARMA;<codigo>;<maquina>;<ts>` se guarda primero en la tabla `alarmas`;
   los `NINGUNA;<ts>` se ignoran. Una alarma repetida no se duplica.
3. Se confirma con `POST /api/v1/alarmas/ack` usando un handle HTTP aparte.
   200 y 409 (ya confirmada) cuentan como éxito; 404 se anota con `ts_ack = -1` y no se reintenta.
4. Con 503/429 se respeta `Retry-After` (+1 s de margen); sin red se espera 1, 2, 4… hasta 30 s.
   Los ack pendientes se reintentan aunque se reinicie el programa.
5. La suscripción se renueva cada 20 s. Si se corta antes de 10 s, reconecta con espera creciente.

## Base de datos (`estacion.db`)

Modo WAL y `busy_timeout` de 2 s. Tablas: `config`, `lecturas_planta`,
`telemetria` (cola con `enviado` e `intentos`) y `alarmas`.
Todo SQL con datos variables usa sentencias preparadas con `?`.

## Cómo se evita perder lecturas

1. Cada lectura se guarda primero en `telemetria` con su `seq` (MAX+1, sobrevive reinicios).
2. Se envía la pendiente más antigua; solo se marca `enviado = 1` si el servidor la confirma (201 o 200).
3. Si el servidor responde 503/429 se respeta `Retry-After`; si falla la red se espera 1, 2, 4… hasta 30 s.
4. Se reutiliza un solo handle de libcurl, con `CURLOPT_TIMEOUT` y `CURLOPT_CONNECTTIMEOUT` de 5 s,
   y se revisan tanto el `CURLcode` como el código HTTP.

## Declaración de uso de IA

Este proyecto se desarrolló con la ayuda de **Claude** (asistente de IA de Anthropic).
La IA se usó para: explicar paso a paso el uso de libcurl y SQLite, proponer la estructura
en módulos (`datos`, `red`, `sensor`, `comandos`), escribir y revisar partes del código C,
el `Makefile` y este README, y ayudar a depurar errores de compilación.
Yo ejecuté el programa contra el servidor del reto, comprobé los resultados y estoy en
capacidad de explicar cada parte del código.

## Sensor

El sensor está simulado (`src/sensor.c`): temperatura entre 25 y 45 °C con una onda lenta
más ruido, y corriente entre 2 y 5 A. Para un DHT22 real basta con cambiar el cuerpo de `sensor_leer()`.
