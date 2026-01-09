![BitStream Banner](images/bitstream-logo-white.png)

# BitStream v1.0

**Cliente FTP para ZX Spectrum**

BitStream es un cliente FTP completo para ZX Spectrum que permite descargar archivos desde servidores FTP a través de WiFi, utilizando un módulo ESP8266/ESP-12 conectado mediante AY-UART bit-banging a 9600 baudios.


> 🇬🇧 **[Read in English](README.md)**

## Características

- **Pantalla de 64 columnas** - Interfaz limpia y legible con salida en colores
- **Comandos FTP estándar** - OPEN, USER, PWD, CD, LS, GET, QUIT
- **Conexión rápida** - `!CONNECT host/ruta usuario [pass]` para acceso en una línea
- **Búsqueda de archivos** - `!SEARCH` para encontrar archivos por patrón y tamaño
- **Descargas en lote** - Descarga múltiples archivos con `GET archivo1 archivo2 archivo3`
- **Barra de progreso** - Feedback visual durante las transferencias
- **Monitorización de conexión** - Detección automática de timeouts y desconexiones
- **Historial de comandos** - Navega comandos anteriores con flechas ARRIBA/ABAJO
- **Operaciones cancelables** - Pulsa EDIT para abortar cualquier operación

## Requisitos

### Hardware
- ZX Spectrum (48K/128K/+2/+3)
- divMMC o interfaz compatible con esxDOS
- Módulo WiFi ESP8266 o ESP-12 conectado al chip AY
- Tarjeta SD con esxDOS

### Software
- esxDOS 0.8.x o superior
- Red WiFi preconfigurada en el módulo ESP (usa [NetManZX](https://github.com/IgnacioMonge/NetManZX) o similar)

## Instalación

1. Copia `BitStream.tap` a tu tarjeta SD
2. Carga con `LOAD ""`
3. O copia el binario compilado para ejecutar directamente desde esxDOS

## Inicio Rápido

```
!CONNECT ftp.ejemplo.com/pub/spectrum anonymous
LS
CD games
GET juego.tap
QUIT
```

## Comandos

### Comandos FTP Estándar

| Comando | Descripción | Ejemplo |
|---------|-------------|---------|
| `OPEN host [puerto]` | Conectar a servidor FTP | `OPEN ftp.scene.org` |
| `USER nombre [pass]` | Login con credenciales | `USER anonymous` |
| `PWD` | Mostrar directorio actual | `PWD` |
| `CD ruta` | Cambiar directorio | `CD /pub/games` |
| `LS [filtro]` | Listar contenido | `LS *.tap` |
| `GET archivo [...]` | Descargar archivo(s) | `GET juego.tap` |
| `QUIT` | Desconectar del servidor | `QUIT` |

### Comandos Especiales

| Comando | Descripción | Ejemplo |
|---------|-------------|---------|
| `!CONNECT` | Conexión rápida con ruta | `!CONNECT ftp.site.com/ruta user pass` |
| `!STATUS` | Mostrar estado de conexión | `!STATUS` |
| `!SEARCH [patrón] [>tamaño]` | Buscar archivos | `!SEARCH *.sna >16000` |
| `!INIT` | Re-inicializar módulo WiFi | `!INIT` |
| `!DEBUG` | Alternar modo debug | `!DEBUG` |
| `HELP` | Mostrar comandos estándar | `HELP` |
| `!HELP` | Mostrar comandos especiales | `!HELP` |
| `CLS` | Limpiar pantalla | `CLS` |
| `ABOUT` | Mostrar info de versión | `ABOUT` |

### Navegación

- **ARRIBA/ABAJO** - Historial de comandos
- **IZQUIERDA/DERECHA** - Mover cursor en línea de entrada
- **EDIT** - Cancelar operación actual
- **ENTER** - Ejecutar comando

## Búsqueda de Archivos

El comando `!SEARCH` permite filtrar por patrón de nombre y tamaño mínimo:

```
!SEARCH *.tap          # Buscar todos los .tap
!SEARCH game           # Buscar archivos que contengan "game"
!SEARCH *.sna >48000   # Buscar .sna mayores de 48KB
!SEARCH >16384         # Buscar cualquier archivo mayor de 16KB
```

## Barra de Estado

La barra de estado inferior muestra:
- **Host** - Servidor conectado (o "---" si desconectado)
- **User** - Nombre de usuario logueado
- **Path** - Directorio remoto actual
- **Indicador** - Estado de conexión (verde=logueado, amarillo=conectado, rojo=desconectado)

## Solución de Problemas

### "No WiFi" al iniciar
- Asegúrate de que el módulo ESP está bien conectado
- Verifica que el WiFi está configurado (usa NetManZX primero)
- Prueba `!INIT` para re-inicializar

### Timeouts de conexión
- El servidor puede tener timeout por inactividad; reconecta con `!CONNECT`
- Comprueba la intensidad de señal WiFi
- Algunos servidores limitan conexiones anónimas

### Errores de transferencia
- Asegúrate de tener espacio suficiente en la SD
- Archivos grandes pueden dar timeout en conexiones lentas
- Usa `!STATUS` para verificar que la conexión está activa

### Comandos que no responden
- Pulsa EDIT para cancelar operaciones bloqueadas
- Prueba `!INIT` para resetear el estado del módulo

## Detalles Técnicos

- **Velocidad**: 9600 bps (AY-UART bit-banging)
- **Protocolo**: FTP modo pasivo
- **Pantalla**: Modo texto 64 columnas (fuente 4x8 píxeles)
- **Buffer**: Buffer circular de 256 bytes para UART
- **Timeouts**: Basados en frames (50Hz) para timing preciso

## Compilar desde Fuentes

Requiere compilador z88dk:

```bash
zcc +zx -vn -SO3 -startup=0 -clib=new -zorg=24576 \
    -pragma-define:CLIB_MALLOC_HEAP_SIZE=0 \
    -pragma-define:CLIB_STDIO_HEAP_SIZE=0 \
    -pragma-define:CRT_STACK_SIZE=512 \
    bitstream.c ay_uart.asm -o BitStream -create-app
```

## Créditos

- **Código**: M. Ignacio Monge Garcia
- **Driver AY-UART**: Basado en código de A. Nihirash
- **Fuente**: Fuente 4x8 de 64 columnas de fuentes comunes ZX

## Licencia

Este proyecto se distribuye bajo licencia MIT. Ver [LICENSE](LICENSE) para más detalles.

## Enlaces

- [NetManZX](https://github.com/IgnacioMonge/NetManZX) - Gestor de redes WiFi para ZX Spectrum
- [esxDOS](http://esxdos.org) - DOS para interfaces divMMC
- [z88dk](https://github.com/z88dk/z88dk) - Kit de desarrollo Z80

---

*BitStream v1.0 - (C) 2025 M. Ignacio Monge Garcia*
