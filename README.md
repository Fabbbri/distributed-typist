# distributed-typist

Proyecto 2 de Principios de Sistemas Operativos (CE4303), desarrollado en C para Linux.
Permite enviar archivos por TCP desde un cliente de consola, cifrándolos carácter por
carácter de C con AES-256-GCM y OpenSSL. El servidor puede almacenar solamente o ejecutar
el descifrado y conteo de palabras con tres procesos OpenMPI. La integración se ha probado
en una computadora con TCP en loopback; la validación en tres equipos físicos está pendiente.

## Preparación

En Debian/Ubuntu, instalar las dependencias y compilar desde la raíz del repositorio:

```sh
sudo apt install build-essential libssl-dev openssl
make
make keygen  # Solo una vez, si todavía no existe la clave
```

Conservar `keys/client.key`: el cliente la reutiliza y será necesaria para descifrar los archivos.
La clave está excluida de Git y `keygen` no sobrescribe una existente.

## Ejecución

Terminal 1, iniciar el servidor:

```sh
make server
```

Terminal 2, abrir el cliente:

```sh
make client
```

Dentro del cliente:

```text
enviar example.txt
salir
```

Colocar los archivos en `input/` o indicar una ruta explícita, como `enviar /tmp/archivo.txt`.
El servidor escucha en `127.0.0.1:5000` y guarda cada envío en `output/cifrado.bin`,
reemplazando el anterior. Detenerlo con `Ctrl+C`.

Para usar otro puerto, configurarlo en ambas terminales:

```sh
make server PORT=6000
make client PORT=6000
```

También se pueden configurar `ADDRESS`, `INPUT_DIR`, `OUTPUT_FILE` y `KEY_FILE`.

## Pruebas y limpieza

```sh
make test   # Ejecuta las pruebas de cifrado y transferencia
make clean  # Elimina ejecutables; conserva la clave y los archivos
```

## Cliente y servidor con tres procesos OpenMPI

En Debian/Ubuntu:

```sh
sudo apt install openmpi-bin libopenmpi-dev
```

Con la clave ya generada, iniciar en la terminal 1:

```sh
make cluster-server
```

En la terminal 2:

```sh
make client
```

Enviar un archivo mediante `enviar example.txt`. El rango 0 atiende TCP y procesa un
fragmento; los rangos 1 y 2 procesan los otros dos. El servidor muestra el equipo, PID
y conteos de cada rango, y la palabra más frecuente global con su cantidad. El cliente
confirma éxito cuando termina el procesamiento. Se generan:

- `output/cifrado.bin`: contenedor original recibido.
- `output/descifrado.txt`: texto autenticado idéntico al original.
- `output/conteos.txt`: diccionario global en formato DTC1 (`cantidad<TAB>palabra`).

Cada envío reemplaza las salidas. `make server` conserva el modo de solo almacenamiento;
utilizar uno de los dos servidores a la vez si comparten puerto. La prueba automática
integral es `make test-cluster`; `make test-mpi` sigue siendo un diagnóstico independiente.
No es necesario conectar la Raspberry Pi Pico H ni el brazo para estas pruebas;
la integración física y la pantalla de selección todavía están pendientes.

Detalles, criterios de conteo y límites: [guía de pruebas MPI](docs/mpi-testing.md).
Detalles del algoritmo: [documentación del cifrado](docs/encryption.md).

## Hardware y componentes pendientes

El diseño actual utiliza una **Raspberry Pi Pico H original, basada en RP2040**,
un brazo de acrílico de MicroJPM y tres servomotores: **dos SG92R y un MG90S**.
La Pico H ejecutará firmware en C con el Pico SDK; Linux y OpenMPI se ejecutan en
las computadoras. La placa seleccionada no tiene Wi-Fi ni Bluetooth integrado.

La conexión prevista es computadora Linux → **USB** → Pico H, y Pico H → **GPIO de
control** → servos del brazo. El brazo presionará físicamente las teclas de un
teclado conectado a la misma computadora que ejecuta el servidor:

| Tecla | Acción prevista en la pantalla de selección |
| --- | --- |
| J | Retroceder en la selección de caracteres |
| K | Confirmar el carácter seleccionado |
| L | Avanzar en la selección de caracteres |

La pantalla de selección y el control físico **todavía están pendientes**. Ya existe
una primera base de comunicación USB: módulo propio del kernel de Linux en C,
biblioteca estática `.a` y firmware para la Pico H que responde `UNCONFIGURED`.
Solo consulta estado; aún no se ha validado con una placa real ni se integra con la
salida del servidor. El módulo y la biblioteca son componentes distintos del firmware.

Se eligió una interfaz USB de fabricante para la consulta inicial. Quedan por definir
los identificadores y el presupuesto USB, las órdenes de movimiento, la asignación y
calibración de servos por articulación, los GPIO, la alimentación, el cableado y la
señal de finalización. La sustitución del ESP32 de la propuesta inicial
es una decisión de hardware; la implementación distribuida parcial y sus dependencias
permanecen vigentes.

Arquitectura prevista, herramientas y decisiones pendientes: [documentación de hardware](docs/hardware.md).

Para compilar la biblioteca y ejecutar sus pruebas nativas, sin conectar la placa:

```sh
make hardware-library
make test-hardware
```

`make driver` compila `driver/typist_usb.ko` con los encabezados del kernel. No lo carga.
`make firmware` requiere Pico SDK y los parámetros USB explícitos. Compilación del
firmware y comprobación física pendiente: [primera versión USB](docs/usb-bringup.md).
