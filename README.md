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
No es necesario conectar el ESP32 para estas pruebas.

Detalles, criterios de conteo y límites: [guía de pruebas MPI](docs/mpi-testing.md).
Detalles del algoritmo: [documentación del cifrado](docs/encryption.md).
