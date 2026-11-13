# Pruebas de cliente, servidor y tres procesos OpenMPI

## 1. Flujo integral en una computadora

Esta prueba utiliza el cliente real, TCP y tres procesos MPI en Linux, sin máquinas
virtuales ni el hardware del dispositivo. El rango 0 atiende al cliente y procesa una
parte; los rangos 1 y 2 procesan las otras dos. Son tres trabajadores efectivos.

En Debian/Ubuntu:

```sh
sudo apt install build-essential libssl-dev openssl openmpi-bin libopenmpi-dev
make
make keygen  # Solo si todavía no existe keys/client.key
```

Desde la raíz del proyecto, crear una entrada cuyo resultado sea fácil de comprobar:

```sh
printf 'aaa aaa comun comun\nbbb bbb comun comun\nccc ccc comun comun\n' > input/nodos.txt
```

Terminal 1:

```sh
make cluster-server
```

Terminal 2:

```sh
make client
```

En la consola del cliente:

```text
enviar nodos.txt
salir
```

El servidor muestra tres mensajes `worker rank=...`, con rangos 0, 1 y 2, el equipo,
PID, bytes procesados y conteos locales. Cada rango procesa 20 bytes y cuatro palabras
para esta entrada, y el resultado global es:

```text
INFO: cluster result word=comun count=6 totalWords=12
```

El orden de los mensajes puede cambiar. En una prueba local los tres tienen el mismo
hostname y distintos PID; en tres equipos deben aparecer los equipos respectivos.
Abrir `htop` o `top` en otra terminal para observar los recursos. Un archivo pequeño
se procesa rápidamente y puede no ser visible en una actualización del monitor.

Comprobar las salidas:

```sh
cmp input/nodos.txt output/descifrado.txt
cat output/conteos.txt
```

`cmp` termina con estado 0 y sin salida si los archivos son idénticos. El original
cifrado queda en `output/cifrado.bin`. El diccionario comienza con `DTC1` y contiene
líneas `cantidad<TAB>palabra`, sin garantizar orden de presentación. El ganador se
muestra en el registro del servidor.

Cada envío reemplaza las salidas. Si se rechaza por autenticación o conteo, se conserva
el cifrado recibido para diagnóstico y los resultados anteriores no se sustituyen.
Un fallo al publicar las salidas puede dejar solo una de ellas actualizada: cada archivo
se publica con `rename` atómico, pero el conjunto no es una transacción. Comprobar la
confirmación del cliente para distinguir un envío fallido.

Detener con `Ctrl+C`; el lanzador MPI puede informar una interrupción. Para una parada
coordinada, enviar `SIGTERM` al PID del coordinador mostrado al inicio: este deja de
escuchar, envía una orden cifrada a los trabajadores y finaliza MPI. `make server`
conserva el modo de solo almacenamiento; no iniciar ambos servidores en el mismo puerto.

Variables: `PORT`, `ADDRESS` (solo loopback), `KEY_FILE`, `OUTPUT_FILE`, `PLAIN_FILE`,
`COUNTS_FILE`, `MPICC`, `MPIEXEC` y `MPI_FLAGS`. Cliente y servidor deben usar el mismo
puerto y clave. El cliente conserva diez segundos de espera por operación de recepción;
un procesamiento largo puede superar su espera final y perder la confirmación aunque
el servidor termine después. Esta etapa está orientada a pruebas locales pequeñas;
no se ha medido escalabilidad ni aceleración.

## 2. Algoritmo y criterios

1. El cliente conserva DTG1: cifra el documento completo carácter por carácter.
2. El rango 0 recibe y guarda el contenedor, y verifica su etiqueta GCM descifrándolo
   a un temporal privado. No usa texto sin autenticar.
3. Divide el texto cerca de los tercios de su longitud, avanzando hasta un separador
   para no cortar palabras. Cifra cada parte como DTG1 independiente con nonce nuevo.
4. Los rangos 1 y 2 reciben sus contenedores por MPI; el rango 0 usa su propia parte.
   Los tres descifran, autentican y construyen diccionarios locales.
5. Los resultados remotos vuelven cifrados y autenticados. El servidor suma todos los
   diccionarios y selecciona el ganador global, sin limitarse a ganadores locales.
6. Guarda los conteos globales y el texto obtenido durante la verificación inicial,
   idéntico al documento original. Después confirma éxito al cliente.

La verificación y el recifrado iniciales añaden trabajo serial. Se eligieron para
reutilizar el cliente y preservar la autenticación global de DTG1; no se afirma que
esta versión reduzca el tiempo de respuesta. Un futuro formato del cliente podría
producir fragmentos autenticados desde el origen y evitar la preparación central.

Una palabra contiene letras y dígitos ASCII o bytes de valor >= 128; el resto separa
palabras. Las mayúsculas ASCII se convierten a minúsculas. UTF-8 se conserva sin
normalización Unicode ni conversión de mayúsculas acentuadas: `ÁRBOL` y `árbol` son
distintos. Algunos signos Unicode también se incluyen por esta regla de bytes.
Los empates se resuelven por el menor orden lexicográfico de bytes.

Límites operativos: 4096 bytes por palabra y 100000 palabras únicas por tabla, además
del límite de 1 GiB del transporte. Un archivo vacío, solo separadores o con pocas
palabras puede dejar fragmentos vacíos; se procesan correctamente y no se inventa un
ganador si no hay palabras. Los límites se rechazan sin truncar palabras. Las fronteras
priorizan integridad, por lo que el reparto no siempre queda equilibrado.

## 3. Pruebas automáticas y diagnóstico

```sh
make test          # Cifrado y TCP del servidor de almacenamiento
make test-cluster  # Cliente TCP real y servidor con tres rangos MPI
make test-mpi      # Diagnóstico independiente: suma colectiva de tres procesos
```

La prueba integral usa clave y salidas efímeras. Comprueba tres rangos que descifran y
cuentan, exactitud del texto y todos los conteos, ganador global, desempates, UTF-8,
puntuación, fronteras de palabras, mensajes de varios bloques, archivos vacíos, clave
incorrecta, contenedor inválido, palabra excesiva, recuperación del servicio, parada
coordinada y configuración de procesos y claves.

`test-mpi` ejecuta `tests/mpiSmoke.c`, sin cliente ni servidor. Solo intercambia números
sintéticos mediante `MPI_Allreduce` y comprueba `1 + 2 + 3 = 6`. `--oversubscribe` permite
tres procesos aunque haya menos ranuras; `--tag-output` identifica sus salidas.
Ejecutar como usuario normal.

## 4. Ejecución en tres computadoras Linux

La integración solo se ha verificado en una computadora. Para la topología de la
propuesta se necesitan tres equipos físicos y validar el flujo allí.

1. Conectar los equipos a una red donde puedan comunicarse entre sí.
2. Instalar OpenMPI compatible, preferiblemente de la misma versión y en la misma ruta,
   y las dependencias C/OpenSSL. Compilar `make bin/clusterServer bin/mpiSmoke` en cada
   copia local del repositorio; utilizar la misma ruta absoluta simplifica el lanzamiento.
3. Proveer la misma clave privada de 32 bytes en cada equipo por un mecanismo seguro,
   con permisos `0600`. No generar claves diferentes ni guardarlas en Git.
4. Configurar SSH sin interacción entre los equipos que usa OpenMPI y verificar las
   rutas de ejecutables y bibliotecas en sesiones no interactivas. Comprobar, por
   ejemplo, `ssh -o BatchMode=yes nodo2 hostname` y los demás accesos entre equipos.
5. Verificar conectividad MPI además de SSH; abrir solo el puerto 5000 no basta.
   No montar directorios compartidos para intercambiar documentos ni resultados.

Crear `/tmp/typist-hosts` en el coordinador, usando nombres resolubles o IP reales y distintos:

```text
nodo1 slots=1
nodo2 slots=1
nodo3 slots=1
```

Diagnóstico previo, desde la raíz del repositorio:

```sh
mpirun --hostfile /tmp/typist-hosts --map-by ppr:1:node --tag-output \
    -np 3 "$PWD/bin/mpiSmoke"
```

Iniciar el servidor distribuido:

```sh
mkdir -p output
mpirun --hostfile /tmp/typist-hosts --map-by ppr:1:node --rank-by node --tag-output \
    -np 3 "$PWD/bin/clusterServer" 127.0.0.1 5000 \
    "$PWD/output/cifrado.bin" "$PWD/output/descifrado.txt" \
    "$PWD/output/conteos.txt" "$PWD/keys/client.key"
```

Las rutas del ejecutable y la clave deben existir en los tres equipos. Las salidas
permanentes solo se crean en el rango 0; los remotos usan sus propios temporales.
Comprobar en el registro que el rango 0 está en el equipo esperado, y ejecutar el
cliente allí con `make client`. El cliente TCP permanece en loopback; no abrirlo a la
red sin completar la protección del canal. No iniciar tres `mpirun` independientes:
los trabajadores deben pertenecer a la misma ejecución.

## 5. Pendientes respecto al enunciado completo

Los documentos, las tareas (incluida la parada) y los diccionarios MPI van cifrados y
autenticados con GCM. Las claves se cargan localmente y al inicio se comprueba que todos
puedan autenticar una prueba del coordinador, sin transmitir la clave.

Las cabeceras y estados TCP, las longitudes MPI y los indicadores de disponibilidad
iniciales siguen en claro. No hay TLS, identidad individual de pares ni protección
contra repetición de mensajes. Falta completar la protección del protocolo para
satisfacer plenamente el cifrado de la información de red. El arranque por SSH no
cifra automáticamente el tráfico de datos MPI.

También quedan la validación en tres equipos, la integración con biblioteca `.a`,
driver propio y ESP32, y la medición del reparto de carga y rendimiento.

## Referencias

- [Lanzamiento de aplicaciones OpenMPI](https://docs.open-mpi.org/en/main/launching-apps/quickstart.html).
- [Distribución de procesos entre equipos](https://docs.open-mpi.org/en/main/launching-apps/scheduling.html).
- [Arranque mediante SSH](https://docs.open-mpi.org/en/main/launching-apps/ssh.html).
