# distributed-typist

Primera etapa del Proyecto 2 de CE4303: transferencia de archivos por sockets TCP en C11
sobre Linux. Solo valida el transporte y el almacenamiento. **No utiliza cifrado ni
autenticación y debe usarse exclusivamente con datos de prueba locales.** Ambos programas
aceptan únicamente direcciones IPv4 numéricas de loopback (`127.0.0.0/8`). No usar documentos
reales ni exponer este transporte mediante túneles o proxies.

Se revisaron `.codex/estandares_de_programacion.md`, `.codex/Proyectos_SO_Proyecto_2.md`,
`.codex/SO_PropuestaP2.md` y las cuatro imágenes de la propuesta. La imagen
`imagenes_proyecto2/capas_generales.png` referenciada por el enunciado no está disponible.
La arquitectura mantiene la conexión cliente-servidor TCP de la propuesta. El cifrado,
OpenMPI, conteo, biblioteca de hardware, driver y ESP32 pertenecen a etapas posteriores;
no se incluyen implementaciones ni simulaciones de esos componentes.

## Compilación

Se requieren un compilador C, las interfaces POSIX de Linux y GNU Make; no hay bibliotecas
externas. Las pruebas usan Bash, `timeout`, `cmp`, `sha256sum`, `truncate`, `mkfifo`,
`tr`, `head`, `grep` y las demás herramientas habituales de Linux.

```sh
make          # Cliente y servidor: bin/client y bin/server
make client   # Solo cliente
make server   # Solo servidor
make clean    # Elimina los tres ejecutables generados, incluido el auxiliar de pruebas
```

Se compila con C11, `-Wall -Wextra -Wpedantic` y tamaños de archivo de 64 bits.

## Ejecución en dos terminales

Desde la raíz del repositorio, terminal 1:

```sh
mkdir -p output
./bin/server 127.0.0.1 5000 output/recibido.txt
```

Terminal 2:

```sh
printf 'Prueba local de transporte TCP.\n' > /tmp/entrada-tcp.txt
./bin/client 127.0.0.1 5000 /tmp/entrada-tcp.txt
cmp /tmp/entrada-tcp.txt output/recibido.txt
sha256sum /tmp/entrada-tcp.txt output/recibido.txt
```

Los argumentos son obligatorios: dirección, puerto decimal (1–65535) y ruta del archivo.
El servidor configura su dirección de escucha y destino; el cliente configura la dirección
del servidor y el origen. El directorio de destino debe existir. El cliente devuelve 0 solo
si recibe la confirmación final de éxito y cierra sus recursos sin errores; devuelve 1
ante errores. El servidor permanece atendiendo clientes secuencialmente tras cada conexión,
incluso si una transferencia falla. Cada éxito reemplaza el archivo de destino anterior.
Para conservar varias transferencias, copiar el resultado o reiniciar con otra ruta.

`Ctrl+C` o `SIGTERM` detienen el servidor con limpieza normal. Si hay una transferencia
activa, primero termina esa atención o vence su espera de E/S. No hay hilos ni procesos por
cliente. Los mensajes técnicos están en inglés y usan los niveles `INFO`, `WARN` y `ERROR`.

## Protocolo DTP1 de esta etapa

TCP es un flujo: los límites de `send()` no se conservan en `recv()`. Se envía una sola
transferencia por conexión y no se transmite el nombre ni la ruta del archivo.

| Paso | Dirección | Formato |
| --- | --- | --- |
| 1 | Cliente → servidor | 12 bytes: ASCII `DTP1` (4) + tamaño sin signo (8), big endian |
| 2 | Servidor → cliente | Un byte: `READY` (1) o estado de error |
| 3 | Cliente → servidor | Exactamente el número de bytes anunciado, sin transformaciones |
| 4 | Cliente → servidor | `shutdown(SHUT_WR)`: EOF de envío, conserva la recepción abierta |
| 5 | Servidor → cliente | Un byte de resultado final y cierre de la conexión |

| Valor | Estado | Significado |
| --- | --- | --- |
| 0 | `SUCCESS` | Contenido completo, archivo cerrado y publicado en el destino |
| 1 | `READY` | Cabecera aceptada y archivo temporal abierto; puede enviarse el cuerpo |
| 2 | `INVALID` | Cabecera inválida/incompleta, tamaño excesivo o bytes adicionales |
| 3 | `IO_ERROR` | Fallo de preparación, escritura, cierre o publicación del archivo |
| 4 | `INCOMPLETE` | Cuerpo incompleto, desconexión o espera agotada durante la recepción |

La cabecera versionada permite rechazar formatos incompatibles; futuros cambios deberán
definir su formato explícitamente. No se envían estructuras C directamente ni se depende
del orden de bytes de la máquina. Un archivo vacío tiene tamaño cero y sigue el mismo flujo.
Los estados desconocidos o inesperados se consideran errores. La confirmación de un error
es un intento: una conexión rota puede impedir que llegue al cliente. La ausencia de
confirmación siempre es fallo para el cliente; si se pierde después de publicar el archivo,
este podría existir en el servidor. No hay reintentos automáticos.

Decisiones de implementación para esta etapa:

- Límite de **1 GiB** por archivo en ambos extremos, validado antes de transmitir el cuerpo.
  Es un límite operativo local, centralizado en `common/protocol.h`, no un requisito del curso.
- Bloques de **16 KiB**, memoria acotada y bucles para envíos/recepciones parciales y `EINTR`.
  Se evita `SIGPIPE` con `MSG_NOSIGNAL` para manejar desconexiones mediante retornos.
- Espera máxima de **10 segundos por operación bloqueante de E/S** en sockets; no es un
  plazo total de transferencia. Un emisor que siga progresando puede ocupar el servidor.
- Solo archivos regulares como origen. Se rechazan directorios, FIFO y tamaños inválidos;
  se detectan cambios de longitud durante el envío. El usuario debe mantener estable el
  contenido: no se crea una instantánea ni se detectan modificaciones que mantengan el tamaño.
- `mkstemp` crea una salida temporal con permisos 0600 junto al destino. Se comprueba el
  cuerpo completo y su EOF, se verifica `fclose` y se publica mediante `rename` atómico.
  Los fallos previos conservan el destino anterior y eliminan el temporal. `SIGKILL` o una
  caída del sistema pueden dejar temporales; no se promete persistencia frente a apagones
  porque no se utiliza `fsync`.
- Los bytes se conservan, incluidos UTF-8, saltos de línea y ceros. No se interpreta el texto
  ni se añade validación de codificación. La integridad se comprueba externamente en pruebas;
  no hay hash ni protección criptográfica dentro del protocolo.

## Organización

| Archivos | Responsabilidad |
| --- | --- |
| `client/client.c`, `client/client.h` | Argumentos, archivo de entrada, conexión y confirmaciones |
| `server/server.c`, `server/server.h` | Escucha, recepción secuencial, publicación y limpieza |
| `common/protocol.c`, `common/protocol.h` | Formato, estados, límites y E/S completa por bloques |
| `Makefile` | Compilación individual/conjunta, pruebas y limpieza |
| `tests/tcpPeer.c`, `tests/run.sh` | Auxiliar C y coordinación de pruebas de integración |

Las funciones internas son `static`; los encabezados exponen solo las interfaces del módulo.
Las funciones están documentadas mediante Doxygen en español, con identificadores camelCase
y mensajes técnicos en inglés, conforme a los estándares del repositorio.

## Pruebas reproducibles

```sh
make test
```

La suite usa exclusivamente loopback y archivos temporales. Inicia los programas como
procesos separados y comprueba sus códigos de salida. Verifica archivos pequeños, texto de
32 MiB y archivos vacíos mediante **`cmp` y SHA-256**, conexiones sucesivas, entradas
inexistentes/no regulares/excesivas, argumentos inválidos, puerto ocupado y conexión rechazada.

El auxiliar C prueba cabeceras y cuerpo enviados byte a byte, cabeceras truncadas o inválidas,
tamaños de red excesivos, cuerpo incompleto, bytes extra, cierre con RST y cliente inactivo
hasta agotar la espera. Verifica que el servidor continúe operativo, conserve el destino
anterior y elimine temporales tras fallos. También se prueban errores de creación/publicación
en el servidor y desconexión del servidor durante el envío, pérdida de confirmación final y
estado de respuesta desconocido en el cliente. Los pares auxiliares solo ejercitan el protocolo
TCP; no representan componentes futuros del proyecto.

La suite suele tardar unos 15 segundos por la prueba de espera agotada y requiere poder abrir
sockets TCP locales. Un resultado `PASS` corresponde a una comprobación ejecutada; errores
o restricciones del entorno deben revisarse en la salida de `make test`.

Verificación realizada el 11 de noviembre de 2026 en Linux: compilación de los tres
ejecutables sin advertencias y `make test` completo con resultado satisfactorio. También
pasaron `bash -n tests/run.sh` y `git diff --check`. La creación de sockets estaba bloqueada
en el entorno restringido; las pruebas TCP se ejecutaron con autorización para usar loopback.
