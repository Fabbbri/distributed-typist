# Primera versión USB: driver, biblioteca y firmware Pico H

Esta versión implementa una **consulta de estado** del dispositivo. Incluye un
módulo propio del kernel de Linux, una biblioteca estática `.a` y firmware en C para
la Raspberry Pi Pico H original, con RP2040. Todavía no mueve servos, no pulsa teclas
y no está conectada a la salida del servidor MPI ni a una pantalla de selección.

## 1. Componentes y contrato

| Componente | Archivos principales | Resultado |
| --- | --- | --- |
| Protocolo y ABI | `include/typistUsbProtocol.h`, `include/typistIoctl.h` | Respuesta USB de ocho bytes y comando ioctl de consulta |
| Driver propio Linux | `driver/typist_usb.c` | `driver/typist_usb.ko`; registra `/dev/typistN` tras reconocer el firmware |
| Biblioteca de usuario | `library/armDevice.c`, `library/armDevice.h` | `build/libtypist.a`: `armOpen`, `armGetStatus` y `armClose` |
| Herramienta de comprobación | `tools/armStatus.c` | `bin/armStatus`, enlazada con la biblioteca `.a` |
| Firmware RP2040 | `firmware/pico/` | Programa `typist_pico` con Pico SDK y TinyUSB |

El módulo se vincula a una **interfaz USB de fabricante** (`0xff`), con VID/PID
explícitos. No utiliza CDC serial ni un driver genérico de TTY. La biblioteca accede
al nodo del módulo propio mediante `ioctl`; la herramienta solo usa esa biblioteca.
El módulo emplea la infraestructura USB del kernel para implementar el protocolo
del dispositivo, siguiendo las [interfaces oficiales de drivers USB Linux](https://docs.kernel.org/driver-api/usb/writing_usb_driver.html).

La clase es una decisión de esta primera implementación; sigue pendiente validar
con el profesor el cumplimiento del requisito académico del driver propio. Los VID/PID
no tienen valores predeterminados ni se consideran identificadores asignados al equipo.
Se deberán elegir para el contexto de uso y coincidir exactamente en firmware y módulo.

La consulta usa el endpoint de control EP0:

| Campo USB | Valor |
| --- | --- |
| `bmRequestType` | `0xc1`: entrada, fabricante, destinatario interfaz |
| `bRequest` | `0x01`: consultar estado |
| `wValue` | `1`: versión del protocolo propio |
| `wIndex` | `0`: interfaz |
| `wLength` | `8`: longitud exacta de respuesta |

La respuesta es `TPH1` (cuatro bytes), versión `1`, estado `UNCONFIGURED` (`1`),
capacidad `STATUS_ONLY` (`1`) y un byte reservado en cero. No contiene punteros ni
enteros multibyte. Driver y biblioteca rechazan versiones, firmas o capacidades
incompatibles. El firmware rechaza otras solicitudes con STALL.

TinyUSB declara los dos endpoints bulk de su interfaz vendor estándar, pero el
protocolo v1 usa únicamente EP0. No hay lectura/escritura de movimiento por bulk.
La respuesta permanece en memoria estática durante la transferencia; el módulo usa
memoria de heap para USB y protege las consultas frente a la desconexión. Los
descriptores abiertos conservan una referencia al contexto y las consultas posteriores
a desconectar devuelven `ENODEV`, sin simular una respuesta satisfactoria.

## 2. Compilación en Linux

Biblioteca y comprobación nativa, sin conectar la Pico ni cargar módulos:

```sh
make hardware-library
make test-hardware
```

Los productos son `build/libtypist.a`, `bin/armStatus` y `bin/armDeviceTest`.
La prueba usa el procesador de peticiones real del firmware y una interceptación de
`ioctl` para inyectar respuestas y errores en la biblioteca. También comprueba rutas
inexistentes, archivos, FIFO y `/dev/null`. **No emula el controlador USB del kernel
ni demuestra una comunicación con una placa física.**

Para compilar el módulo con los encabezados del kernel instalado:

```sh
make driver
```

`KERNEL_BUILD` permite indicar otro directorio de encabezados. Kbuild determina el
compilador y las opciones del kernel, en lugar de imponer el estándar C11 de usuario.
Si informa una diferencia de compilador, utilizar el mismo que muestran los encabezados,
por ejemplo mediante `make driver CC=...` con el ejecutable correspondiente.
`make clean-driver` limpia solo los productos del módulo; `make clean` limpia los
ejecutables y la biblioteca de usuario y conserva el árbol de construcción del firmware.

## 3. Compilación del firmware

Se necesita el Pico SDK con su submódulo TinyUSB, CMake, un compilador Arm para
RP2040 y las bibliotecas/encabezados del entorno embebido. El SDK incluye soporte
interno C++, aunque las fuentes del firmware del proyecto están escritas en C y el
enlace utiliza C. Consultar la [preparación oficial del Pico SDK](https://github.com/raspberrypi/pico-sdk).

La placa configurada es **`PICO_BOARD=pico`**, que corresponde también a la Pico H
original, con **`PICO_PLATFORM=rp2040`**. CMake rechaza otra placa o plataforma. La
configuración no habilita UART ni salida estándar USB, y no inicializa GPIO de servos
ni LED. TinyUSB es atendido desde un bucle cooperativo, sin incorporar FreeRTOS.

Definir antes de compilar:

- `PICO_SDK_PATH`: ruta a la copia completa del SDK.
- `USB_VID` y `USB_PID`: identificadores elegidos para este dispositivo.
- `USB_POWER_MA`: presupuesto que declarará el dispositivo USB en `bMaxPower`.
  USB 2.0 lo representa en unidades de 2 mA; la configuración exige un número par de
  2 a 500 mA. Es un dato del dispositivo USB que debe validarse antes de la prueba
  física; no dimensiona ni decide la fuente de alimentación de los servos.

Con esas variables ya definidas en la terminal:

```sh
make firmware PICO_SDK_PATH="${PICO_SDK_PATH:?definir ruta del SDK}" \
    USB_VID="${USB_VID:?definir VID}" USB_PID="${USB_PID:?definir PID}" \
    USB_POWER_MA="${USB_POWER_MA:?definir presupuesto USB}"
```

Los archivos se generan en `build/pico`. La generación UF2 depende de las herramientas
del SDK, incluido `picotool`. Para verificar únicamente ELF sin disponer de picotool:

```sh
make firmware PICO_SDK_PATH="${PICO_SDK_PATH:?definir ruta del SDK}" \
    USB_VID="${USB_VID:?definir VID}" USB_PID="${USB_PID:?definir PID}" \
    USB_POWER_MA="${USB_POWER_MA:?definir presupuesto USB}" \
    FIRMWARE_CMAKE_FLAGS=-DPICO_NO_PICOTOOL=1
```

Esta última opción no genera UF2. Para habilitarlo después, configurar explícitamente
`FIRMWARE_CMAKE_FLAGS=-DPICO_NO_PICOTOOL=0` o usar otro directorio mediante `FIRMWARE_BUILD`.
No se ha grabado el firmware en una placa durante esta implementación.

## 4. Comprobación física pendiente

Después de elegir y validar los parámetros USB, compilar el firmware correspondiente,
cargarlo en la Pico H y conectarla al equipo Linux. El procedimiento de carga se debe
realizar con los artefactos del firmware del proyecto, sin confundir BOOTSEL con la
interfaz de ejecución del programa.

Cargar el módulo con los mismos IDs usados para ese firmware:

```sh
sudo insmod driver/typist_usb.ko \
    vendorId="${USB_VID:?definir VID}" productId="${USB_PID:?definir PID}"
lsmod | grep typist_usb
ls /dev/typist*
```

El módulo debe cargarse manualmente: no anuncia un alias universal ni toma interfaces
de cualquier fabricante. Si reconoce la firma del protocolo crea `/dev/typistN`; usar
el número observado, por ejemplo:

```sh
sudo ./bin/armStatus /dev/typist0
```

La salida prevista es:

```text
INFO: Pico H protocol=1 state=UNCONFIGURED capabilities=STATUS_ONLY
```

Esta salida confirma solo la consulta. Los permisos permanentes del nodo, la firma
del módulo si el equipo la requiere, la enumeración USB real y los casos de desconexión
y reconexión aún deben verificarse en el equipo. Después de cerrar las herramientas,
`sudo rmmod typist_usb` retira el módulo. No se ha cargado automáticamente en esta sesión.

## 5. Evidencia y trabajo restante

Se compiló el módulo para los encabezados Linux `6.17.0-29-generic`. Se verificó el ELF
para RP2040 con Pico SDK `2.3.1`, su TinyUSB fijado por el submódulo y Arm GCC `13.2.1`,
sin generación UF2 (`PICO_NO_PICOTOOL=1`). Los parámetros `0xcafe`, `0x4000` y `100` se
usaron exclusivamente para comprobar la compilación, sin cargarlos en un dispositivo
ni adoptarlos como identidad o consumo del hardware definitivo.

Las pruebas nativas cubren la validación del contrato, el rechazo de respuestas
incompatibles y la propagación de errores. La comprobación física USB, la carga del
módulo y el firmware, y cualquier movimiento siguen pendientes.

El siguiente paso funcional es verificar esa consulta con la Pico real. Después se
podrá ampliar el protocolo al control físico, una vez definidos articulaciones, GPIO,
alimentación, cableado, calibración y sincronización con la pantalla J/K/L. La API
actual no contiene órdenes de movimiento y no permite declarar un trabajo mecánico
completado. El procesamiento distribuido existente conserva su comportamiento.
