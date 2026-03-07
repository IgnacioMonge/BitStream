![BitStream Banner](images/bitstream-logo-white.png)

# BitStream v1.2.0

**Cliente FTP para ZX Spectrum**

BitStream es un cliente FTP completo para ZX Spectrum que permite descargar archivos desde servidores FTP a traves de WiFi, utilizando un modulo ESP8266/ESP-12. Soporta dos interfaces UART: **divMMC/divTiesus** a 115200 baudios y **AY-UART bit-banging** a 9600 baudios.


> [Read in English](README.md)

## Caracteristicas

- **Soporte dual UART** - divMMC/divTiesus (115200 baud) o AY bit-banging (9600 baud)
- **Pantalla de 64 columnas** - Interfaz limpia y legible con salida en colores, fuente comprimida 4x8
- **Comandos FTP estandar** - OPEN, USER, PWD, CD, LS, GET, QUIT
- **Conexion rapida** - `!CONNECT host/ruta usuario [pass]` para acceso en una linea
- **Login interactivo** - Solicita usuario/password con entrada enmascarada y valores anonimos por defecto
- **Busqueda de archivos** - `!SEARCH` para encontrar archivos por patron y tamano
- **Descargas en lote** - Descarga multiples archivos con `GET archivo1 archivo2 archivo3`
- **Barra de progreso** - Feedback visual durante las transferencias
- **Monitorizacion de conexion** - Deteccion automatica de timeouts y desconexiones
- **Historial de comandos** - Navega comandos anteriores con flechas ARRIBA/ABAJO (4 entradas)
- **Operaciones cancelables** - Pulsa EDIT para abortar cualquier operacion
- **Integracion esxDOS** - Escritura directa a SD via traps RST 0x08

[![BitStream1](images/BTS1_1.png)](images/BTS1.png) [![BitStream2](images/BTS2_1.png)](images/BTS2.png) [![BitStream3](images/BTS3_1.png)](images/BTS3.png)


## Requisitos

### Hardware
- ZX Spectrum (48K/128K/+2/+3)
- divMMC o interfaz compatible con esxDOS
- Modulo WiFi ESP8266 o ESP-12 conectado via:
  - **UART divMMC/divTiesus** (directamente en la interfaz, 115200 baud) - recomendado
  - **Chip AY-3-8912** (bit-banging, 9600 baud)
- Tarjeta SD con esxDOS

### Software
- esxDOS 0.8.x o superior
- Red WiFi preconfigurada en el modulo ESP (usa [NetManZX](https://github.com/IgnacioMonge/NetManZX) o similar)

## Instalacion

1. Copia el archivo `.tap` correspondiente a tu tarjeta SD:
   - `BitStream_divTiesus.tap` para UART divMMC/divTiesus
   - `BitStream_AY.tap` para AY bit-banging
2. Carga con `LOAD ""`
3. O copia el binario compilado para ejecutar directamente desde esxDOS

## Inicio Rapido

```
!CONNECT ftp.ejemplo.com/pub/spectrum anonymous
LS
CD games
GET juego.tap
QUIT
```

O conecta paso a paso:

```
OPEN ftp.scene.org
```

BitStream pedira usuario y contrasena de forma interactiva. Pulsa ENTER para aceptar los valores anonimos por defecto.

## Comandos

### Comandos FTP Estandar

| Comando | Descripcion | Ejemplo |
|---------|-------------|---------|
| `OPEN host[:puerto]` | Conectar a servidor FTP | `OPEN ftp.scene.org` |
| `USER nombre [pass]` | Login con credenciales | `USER anonymous` |
| `PWD` | Mostrar directorio actual | `PWD` |
| `CD ruta` | Cambiar directorio | `CD /pub/games` |
| `LS [filtro]` | Listar contenido | `LS *.tap` |
| `GET archivo [...]` | Descargar archivo(s) | `GET juego.tap` |
| `QUIT` | Desconectar del servidor | `QUIT` |

### Comandos Especiales

| Comando | Descripcion | Ejemplo |
|---------|-------------|---------|
| `!CONNECT` | Conexion rapida con ruta | `!CONNECT ftp.site.com/ruta user pass` |
| `!STATUS` | Mostrar estado de conexion | `!STATUS` |
| `!SEARCH [patron] [>tamano]` | Buscar archivos | `!SEARCH *.sna >16000` |
| `!INIT` | Re-inicializar modulo WiFi | `!INIT` |
| `HELP` | Mostrar comandos estandar | `HELP` |
| `!HELP` | Mostrar comandos especiales | `!HELP` |
| `!CLS` | Limpiar pantalla | `!CLS` |
| `!ABOUT` | Mostrar info de version | `!ABOUT` |

### Navegacion

- **ARRIBA/ABAJO** - Historial de comandos
- **IZQUIERDA/DERECHA** - Mover cursor en linea de entrada
- **EDIT** - Cancelar operacion actual
- **ENTER** - Ejecutar comando

## Busqueda de Archivos

El comando `!SEARCH` permite filtrar por patron de nombre y tamano minimo:

```
!SEARCH *.tap          # Buscar todos los .tap
!SEARCH game           # Buscar archivos que contengan "game"
!SEARCH *.sna >48000   # Buscar .sna mayores de 48KB
!SEARCH >16384         # Buscar cualquier archivo mayor de 16KB
```

[![BitStream4](images/BTS4_1.png)](images/BTS4.png) [![BitStream5](images/BTS5_1.png)](images/BTS5.png) [![BitStream6](images/BTS6_1.png)](images/BTS6.png)

## Barra de Estado

La barra de estado inferior muestra:
- **Host** - Servidor conectado (o "---" si desconectado)
- **User** - Nombre de usuario logueado
- **Path** - Directorio remoto actual
- **Indicador** - Estado de conexion (verde=logueado, amarillo=conectado, rojo=desconectado)

## Solucion de Problemas

### "No WiFi" al iniciar
- Asegurate de que el modulo ESP esta bien conectado
- Verifica que el WiFi esta configurado (usa NetManZX primero)
- Prueba `!INIT` para re-inicializar

### Timeouts de conexion
- El servidor puede tener timeout por inactividad; reconecta con `!CONNECT`
- Comprueba la intensidad de senal WiFi
- Algunos servidores limitan conexiones anonimas

### Errores de transferencia
- Asegurate de tener espacio suficiente en la SD
- Archivos grandes pueden dar timeout en conexiones lentas
- Usa `!STATUS` para verificar que la conexion esta activa

### Comandos que no responden
- Pulsa EDIT para cancelar operaciones bloqueadas
- Prueba `!INIT` para resetear el estado del modulo

## Detalles Tecnicos

- **UART**: divMMC/divTiesus a 115200 bps, o AY bit-banging a 9600 bps
- **Protocolo**: FTP modo pasivo (CIPMUX=1, socket 0=control, 1=datos)
- **Pantalla**: Modo texto 64 columnas (fuente comprimida 4x8, 298 bytes)
- **Buffer**: Buffer circular de 2048 bytes para UART
- **Memoria**: ORG 24000, stack de 256 bytes, recorte BSS via `__data_compiler_tail`
- **Timeouts**: Basados en frames (50Hz) para timing preciso

## Arquitectura

BitStream v1.2.0 usa un diseno modular SCU (Single Compilation Unit):

```
src/
  main_build.c    # Orquestador SCU (#includes todos los modulos)
  globals.c       # Estado global, constantes, strings compartidos
  ui.c            # Video, widgets, barra de estado, zona de entrada
  comms.c         # Ring buffer, helpers UART, init ESP, TCP
  ftp.c           # Protocolo FTP, esxDOS, descarga, listado
  commands.c      # Parser de comandos, ayuda, estado
  main.c          # Init pantalla, bucle principal
include/
  bitstream.h     # Header maestro (constantes, externs, decl ASM)
  font64_data.h   # Fuente 4px (96 chars, nibbles empaquetados)
asm/
  bitstream_asm.asm     # Rutinas ASM core (pantalla, ring buffer, render)
  divtiesus_uart.asm    # Driver UART divMMC (115200 baud)
  ay_uart.asm           # Driver UART AY bit-bang (9600 baud)
```

Las rutinas criticas (scroll de pantalla, ring buffer, operaciones de string, renderizado de texto) estan implementadas en ensamblador Z80 para rendimiento en la CPU de 3.5MHz.

## Compilar desde Fuentes

Requiere compilador [z88dk](https://github.com/z88dk/z88dk).

```bash
# Compilar version divMMC (por defecto)
make

# Compilar version AY bit-bang
make ay

# Compilar ambas versiones
make both

# Build de release (optimizacion agresiva)
make release
```

## Creditos

- **Codigo**: M. Ignacio Monge Garcia
- **Driver AY-UART**: Basado en codigo de A. Nihirash
- **Fuente**: Fuente comprimida 4x8 de 64 columnas

## Licencia

Este proyecto se distribuye bajo licencia MIT. Ver [LICENSE](LICENSE) para mas detalles.

## Enlaces

- [NetManZX](https://github.com/IgnacioMonge/NetManZX) - Gestor de redes WiFi para ZX Spectrum
- [esxDOS](http://esxdos.org) - DOS para interfaces divMMC
- [z88dk](https://github.com/z88dk/z88dk) - Kit de desarrollo Z80

---

*BitStream v1.2.0 - (C) 2026 M. Ignacio Monge Garcia*
