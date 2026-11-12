# distributed-typist

Proyecto 2 de Principios de Sistemas Operativos (CE4303), desarrollado en C para Linux.
Actualmente permite enviar archivos por TCP desde un cliente de consola, cifrándolos con
AES-256-GCM y OpenSSL. El servidor los almacena cifrados; el descifrado y el procesamiento
distribuido siguen pendientes. Esta versión funciona únicamente en loopback para pruebas locales.

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

Detalles del algoritmo: [documentación del cifrado](docs/encryption.md).
