![BitStream Banner](images/bitstream-logo-white.png)

# BitStream
## Cliente FTP para ZX Spectrum

[🇬🇧 English Version](README.md)

Cliente FTP completo con conectividad WiFi ESP8266/ESP-12 y interfaz de 64 columnas.

---

## Características

### Conectividad
- **WiFi ESP8266/ESP-12** vía AY-UART bit-banging a 9600 baudios
- **Ring buffer de 512 bytes** para manejo eficiente de datos
- **Monitoreo automático** de conexión en tiempo real
- **Detección de desconexión** (timeout, cierre remoto)

### Protocolo FTP
- **OPEN** - Conectar a servidor FTP
- **USER** - Login con usuario/password
- **PWD** - Mostrar directorio actual (con retry automático)
- **LS** / **LIST** - Listar archivos y directorios
- **CD** - Cambiar directorio
- **GET** - Descargar archivos (soporte batch y comillas)
- **PUT** - Subir archivos al servidor
- **QUIT** - Cerrar conexión

### Interfaz de Usuario
- **64 columnas** con fuente custom de 4x8 píxeles
- **Rendering optimizado** con fast-path para líneas completas
- **Historial de comandos** (↑/↓ para navegar)
- **Indicadores visuales** de estado WiFi/FTP
- **Barra de estado** permanente
- **Cancelación** con tecla EDIT

### Transferencias
- **Descarga múltiple**: `GET file1.txt file2.zip file3.rar`
- **Nombres con espacios**: `GET "Manual del Usuario.pdf"`
- **Barra de progreso** en tiempo real
- **Estadísticas**: velocidad, tiempo, bytes transferidos
- **Reintentos automáticos** en caso de error

---

## Requisitos Hardware

1. **ZX Spectrum 48K/128K**
2. **Interfaz AY-3-8912**
3. **Módulo WiFi ESP8266 o ESP-12**
   - Conectado a pines AY (UART bit-banging)
   - Configurado a 9600 baudios

---

## Comandos Rápidos

### Conexión Rápida
```
!CONNECT ftp.servidor.com/path usuario password
```
Conecta, logea y cambia al directorio en un solo comando.

### Comandos Especiales
```
!HELP      Ayuda sobre comandos especiales
HELP       Ayuda sobre comandos FTP estándar
STATUS     Estado de conexión WiFi/FTP
ABOUT      Información del programa
CLS        Limpiar pantalla
```

### Ejemplos de Uso
```
OPEN ftp.gnu.org
USER anonymous zx@spectrum.net
CD /gnu/gcc
LS
GET "gcc manual.pdf"
QUIT
```

---

## Novedades v1.1

### ✨ Mejoras de Rendimiento
- Ring buffer ampliado (256→512 bytes)
- Rendering 3-4x más rápido en líneas completas
- Teclado más responsive (40ms vs 120ms anteriores)

### 🎯 Nuevas Características
- Soporte de comillas en nombres de archivo
- Detección de sesión ya iniciada (USER)
- PWD con retry automático (8s timeout)

### 🐛 Correcciones
- Connection alive detection mejorada
- Parsing robusto de argumentos
- ~135 bytes de código optimizado

---

## Compilación

### Requisitos
- **Z88DK** (zcc)
- **Make** (opcional, se puede usar batch)

### Build
```bash
make
```

O usando el script batch:
```batch
build.bat
```

Genera **BitStream.tap** (~40KB)

---

## Configuración ESP8266

El módulo ESP debe estar configurado:
- **Baud rate**: 9600
- **Modo multi-conexión**: Habilitado (AT+CIPMUX=1)
- **Conectado a red WiFi**

BitStream incluye inicialización automática inteligente.

---

## Notas Técnicas

### Memoria
- **Código**: ~40KB compilado
- **Ring buffer**: 512 bytes
- **Buffers FTP**: ~2KB (comandos, respuestas, datos)
- **Compatible**: 48K y 128K (código en memoria principal)

### Arquitectura
- **UART bit-banging** vía registros AY-3-8912
- **Rendering 64 columnas** optimizado con fast-path
- **Ring buffer circular** para recepción UART
- **Buffering de escritura** para transferencias rápidas

### Limitaciones
- Modo pasivo FTP únicamente (PASV)
- Nombres 8.3 en archivos locales (esxDOS)
- Sin soporte SSL/TLS (FTP plain)

---

## Créditos

**Autor**: M. Ignacio Monge García  
**Año**: 2025  
**Licencia**: [Especificar]

Basado en:
- **espATZX** (WiFi UART)
- **Z88DK** (compilador)
- **esxDOS** (sistema de archivos)

---

## Soporte

Para bugs, sugerencias o contribuciones:
[Incluir contacto/repositorio]

---

[🇬🇧 Read in English](README.md)
