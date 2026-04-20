![BitStream Banner](images/bitstream-logo-white.png)

# BitStream v1.3.0

**Cliente FTP para ZX Spectrum**

BitStream es un cliente FTP por WiFi para ZX Spectrum que utiliza un modulo ESP8266/ESP-12. Soporta dos backends UART: **divMMC/divTiesus** a 115200 baudios y **AY-3-8912 bit-banging** a 9600 baudios. La version 1.3.0 se centra en hacer las transferencias mas seguras, pulir el renderizado y dejar un sistema de build mas limpio para release.


> [Read in English](README.md)

## Caracteristicas

- **Soporte dual UART** - divMMC/divTiesus (115200 baud) o AY bit-banging (9600 baud)
- **Interfaz de 64 columnas** - fuente comprimida 4x8 con salida coloreada
- **Barra de estado y progreso a doble altura** - mejor visibilidad y menos flicker
- **Comandos FTP estandar** - `OPEN`, `USER`, `PWD`, `CD`, `LS`, `GET`, `QUIT`
- **Conexion rapida** - `!CONNECT host[:port][/ruta] user [pass]`
- **Login interactivo** - password enmascarada con `UP` para alternar visibilidad
- **Filtros de listado** - `LS -d` para directorios, `LS -f` para archivos
- **Filtros de busqueda** - `!SEARCH [patron] [>tamano]`
- **Nombres entre comillas** - soporta archivos con espacios en los argumentos
- **Monitorizacion de conexion** - detecta timeouts, desconexiones y sesiones rotas
- **Descargas seguras** - los ficheros parciales se borran automaticamente al fallar
- **Historial y edicion en linea** - `UP/DOWN/LEFT/RIGHT/BACKSPACE`
- **Integracion esxDOS** - escritura directa a SD via traps RST 0x08

[![BitStream1](images/BTS1_1.png)](images/BTS1.png) [![BitStream2](images/BTS2_1.png)](images/BTS2.png) [![BitStream3](images/BTS3_1.png)](images/BTS3.png)


## Requisitos

### Hardware
- ZX Spectrum (48K/128K/+2/+3)
- divMMC o interfaz compatible con esxDOS
- Modulo ESP8266 o ESP-12 conectado por:
  - **UART divMMC/divTiesus** (directamente en la interfaz, 115200 baud) - recomendado
  - **Chip AY-3-8912** (bit-banging, 9600 baud)
- Tarjeta SD con esxDOS

### Software
- esxDOS 0.8.x o superior
- Red WiFi preconfigurada en el modulo ESP (usa [NetManZX](https://github.com/IgnacioMonge/NetManZX) o similar)

## Instalacion

Descarga los binarios de release segun tu hardware:

- `BitStream_divTiesus.tap` para UART divMMC/divTiesus
- `BitStream_AY.tap` para AY bit-banging

Copia el `.tap` a tu tarjeta SD y cargalo con `LOAD ""`, o ejecuta el binario generado directamente desde esxDOS si lo prefieres.

## Inicio Rapido

```text
!CONNECT ftp.ejemplo.com/pub/spectrum anonymous
LS -f
GET "juego.tap"
QUIT
```

O conecta paso a paso:

```text
OPEN ftp.scene.org
```

BitStream pedira usuario y contrasena de forma interactiva. Pulsa `ENTER` para aceptar los valores anonimos por defecto. Durante la entrada enmascarada, pulsa `UP` para alternar visibilidad.

## Comandos

### Comandos FTP Estandar

| Comando | Descripcion | Ejemplo |
|---------|-------------|---------|
| `OPEN host[:port]` | Conectar a servidor FTP | `OPEN ftp.scene.org` |
| `USER name [pass]` | Login con credenciales | `USER anonymous` |
| `PWD` | Mostrar directorio actual | `PWD` |
| `CD path` | Cambiar directorio | `CD /pub/games` |
| `LS [filtro]` | Listar contenido (`-d` / `-f`) | `LS -f` |
| `GET archivo` | Descargar un archivo | `GET "Manual del Usuario.pdf"` |
| `QUIT` | Desconectar del servidor | `QUIT` |

### Comandos Especiales

| Comando | Descripcion | Ejemplo |
|---------|-------------|---------|
| `!CONNECT` | Conexion rapida con ruta opcional | `!CONNECT ftp.site.com/path user pass` |
| `!STATUS` | Mostrar estado de conexion | `!STATUS` |
| `!SEARCH [patron] [>tamano]` | Buscar archivos | `!SEARCH *.tap >16000` |
| `!INIT` | Re-inicializar el modulo WiFi | `!INIT` |
| `HELP` | Mostrar comandos estandar | `HELP` |
| `!HELP` | Mostrar comandos especiales | `!HELP` |
| `!CLS` | Limpiar pantalla | `!CLS` |
| `!ABOUT` | Mostrar informacion de version | `!ABOUT` |

### Navegacion

- **UP/DOWN** - historial de comandos
- **LEFT/RIGHT** - mover cursor en la linea de entrada
- **BREAK** - cancelar la operacion actual
- **ENTER** - ejecutar comando

## Filtros de Busqueda y Listado

`LS` y `!SEARCH` soportan filtros adicionales:

```text
LS -d                 # Solo directorios
LS -f                 # Solo archivos
!SEARCH *.tap         # Patron de nombre
!SEARCH >16384        # Tamano minimo
!SEARCH *.sna >48000  # Patron + tamano minimo
```

BitStream tambien conserva los bytes UTF-8 entrantes hasta la conversion final a ASCII, lo que mejora los listados de nombres con acentos o caracteres no ASCII.

[![BitStream4](images/BTS4_1.png)](images/BTS4.png) [![BitStream5](images/BTS5_1.png)](images/BTS5.png) [![BitStream6](images/BTS6_1.png)](images/BTS6.png)


## Barra de Estado

La zona inferior de estado muestra:
- **Host** - servidor conectado (o `---` si esta desconectado)
- **User** - nombre de usuario autenticado
- **Path** - directorio remoto actual
- **Indicador** - estado de conexion (verde=logueado, amarillo=conectado, rojo=desconectado)

La version 1.3.0 usa renderizado a doble altura tanto para la barra de estado como para la barra de progreso, con actualizacion parcial para reducir flicker.

## Solucion de Problemas

### "No WiFi" al arrancar
- Asegurate de que el modulo ESP esta bien conectado
- Verifica que el WiFi esta configurado (usa NetManZX primero)
- Prueba `!INIT` para re-inicializar el modulo

### Timeouts de conexion
- El servidor puede tener timeout por inactividad; reconecta con `!CONNECT`
- Comprueba la intensidad de senal WiFi
- Tras una pausa larga en listados paginados, BitStream ahora valida el canal de control antes de asumir que sigue vivo

### Errores de transferencia
- Asegurate de tener espacio libre suficiente en la SD
- Las transferencias fallidas o canceladas borran automaticamente el archivo incompleto local
- Usa `!STATUS` para verificar que la conexion de control sigue activa

### Comandos que no responden
- Pulsa `BREAK` para cancelar operaciones bloqueadas
- Prueba `!INIT` para resetear el estado del ESP

## Detalles Tecnicos

- **UART**: divMMC/divTiesus a 115200 bps, o AY bit-banging a 9600 bps
- **Protocolo**: FTP en modo pasivo (`CIPMUX=1`, socket 0=control, 1=datos)
- **Pantalla**: modo texto de 64 columnas con fuente comprimida 4x8
- **Buffer**: ring buffer de 2048 bytes para trafico UART
- **Pipeline de build**: compresion BPE -> build -> restore -> trim de BSS
- **Memoria**: ORG 24000, stack CRT de 512 bytes, trim de BSS via `__data_compiler_tail`
- **Timeouts**: basados en frames (50Hz) para timing preciso

## Arquitectura

BitStream v1.3.0 mantiene una estructura modular SCU (Single Compilation Unit):

```text
src/
  main_build.c         # Orquestador SCU (#includes de todos los modulos)
  globals.c            # Estado global, constantes, strings compartidos
  ui.c                 # Video, widgets, barra de estado, zona de entrada
  comms.c              # Ring buffer, helpers UART, init ESP, TCP
  ftp.c                # Protocolo FTP, esxDOS, descargas, listados
  commands.c           # Parser de comandos, ayuda, estado
  main.c               # Init de pantalla, banner y bucle principal
  bitstream_copt.rul   # Reglas copt orientadas a tamano
include/
  bitstream.h          # Header maestro
  font64_data.h        # Datos de la fuente 4px
asm/
  bitstream_asm.asm    # Rutinas ASM core
  divtiesus_uart.asm   # Driver UART divMMC
  ay_uart.asm          # Driver AY bit-bang
tools/
  bpe_compress.py      # Compresor de strings en build
  bpe_analyze.py       # Helper de analisis BPE
```

Las rutinas criticas, como scroll, ring buffer, helpers de string y renderizado de texto, siguen implementadas en ensamblador Z80 para rendir bien en la CPU de 3.5MHz.

## Compilar desde Fuentes

Requiere:
- [z88dk](https://github.com/z88dk/z88dk)
- `python3` para el paso BPE del build

```bash
# Compilar version divMMC (por defecto)
make

# Compilar version AY bit-bang
make ay

# Compilar ambas versiones
make both

# Build release (divMMC)
make release

# Build release (AY)
make release-ay
```

Todos los artefactos se generan en `build/`.

## Creditos

- **Codigo**: M. Ignacio Monge Garcia
- **Driver AY-UART**: Basado en codigo de A. Nihirash
- **Fuente**: Fuente comprimida 4x8 de 64 columnas

## Licencia

Este proyecto se distribuye bajo licencia MIT. Ver [LICENSE](LICENSE) para mas detalles.

## Enlaces

- [NetManZX](https://github.com/IgnacioMonge/NetManZX) - Gestor WiFi para ZX Spectrum
- [esxDOS](http://esxdos.org) - DOS para interfaces divMMC
- [z88dk](https://github.com/z88dk/z88dk) - Kit de desarrollo Z80

---

*BitStream v1.3.0 - (C) 2026 M. Ignacio Monge Garcia*
