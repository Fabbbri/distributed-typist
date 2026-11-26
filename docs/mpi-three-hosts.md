# Cliente y servidor con tres computadoras físicas

Esta guía reproduce la prueba del 25 de noviembre de 2026: un proceso MPI en cada
computadora, con el cliente y el servidor reales. La preparación de SSH, las
dependencias y la instalación desde fuentes se explican con más detalle en
[mpi-two-hosts.md](mpi-two-hosts.md).

## 1. Equipos y rutas de la prueba

| Dato | HOST / rango 0 | Remoto / rango 1 | Melissa / rango 2 |
| --- | --- | --- | --- |
| Hostname | `jian-Victus` | `administradorce-OptiPlex-7000` | `meliubuntu` |
| Usuario | `jian` | `user` | `melissa` |
| IPv4 | `172.18.37.0` | `172.18.238.179` | `172.18.100.57` |
| Repositorio | `/home/jian/Desktop/distributed-typist` | `/home/user/distributed-typist` | `/home/melissa/Documents/sistemasOperativos/proyecto2/distributed-typist` |
| OpenMPI | `build/openmpi-4.1.6` dentro del repositorio | `build/openmpi-4.1.6` dentro del repositorio | `/opt/openmpi-4.1.6` |

El rango 0 atiende al cliente y también cuenta palabras. Los rangos 1 y 2 cuentan
los otros fragmentos. El cliente permanece en el host y utiliza `127.0.0.1:5000`.
Los trabajadores se comunican por MPI a través de la red.

Las IP pueden cambiar después de reiniciar. Consultar `hostname -I` en cada equipo
y actualizar las direcciones del archivo `hosts`, la configuración SSH y los
argumentos `--host` de los comandos siguientes si corresponde.

La ruta `/home/melissa/mpi-build/openmpi-4.1.6` es la carpeta de fuentes de OpenMPI;
**no es la carpeta del proyecto ni su prefijo de instalación**.

## 2. Preparación inicial

Estos pasos ya se realizaron para los equipos de la tabla. Para repetir la prueba
sin reinstalar, pasar a la sección 4.

### 2.1. SSH y código

Desde el **HOST**, ambas conexiones deben funcionar sin pedir contraseña:

```sh
ssh -o BatchMode=yes user@172.18.238.179 hostname
ssh -o BatchMode=yes melissa@172.18.100.57 hostname
```

En una instalación nueva, preparar el servicio SSH y usar `ssh-copy-id` como se
describe en la guía de dos equipos. No es necesario dejar abierta una sesión SSH.

En cada copia del repositorio, comprobar `git rev-parse HEAD` y `git status --short`.
Las tres usaban la revisión `58219093633454795e24baea3f1f4a831d047e9a` para esta prueba,
sin cambios locales en el código C. Al modificar el programa, sincronizar las copias
y recompilar en los tres equipos.

### 2.2. Compatibilidad de OpenMPI

Los tres equipos utilizan OpenMPI 4.1.6 con los componentes `pmix3x`, `hwloc201`
y `libevent2022`; PMIx informa `3.2.5a1 (embedded in Open MPI)`. En Melissa se
comprobó la instalación existente de `/opt` y funcionó, sin recompilar OpenMPI.
Los otros equipos usan la instalación preparada en `build/`.

Para comprobarlo en cada equipo, ejecutar `bin/ompi_info --param pmix all`,
`bin/ompi_info --config` y `bin/ompi_info --all` dentro de su prefijo de instalación.
No basta comparar únicamente `mpirun --version`: la preparación anterior encontró
fallos al combinar otras bibliotecas auxiliares.

Si se necesita reconstruir una instalación compatible, la sección 5 de
[mpi-two-hosts.md](mpi-two-hosts.md#5-descargar-e-instalar-openmpi-416) contiene el
archivo fuente, su SHA-256 y las opciones usadas con las bibliotecas internas.
Después habrá que ajustar las rutas del compilador y del enlace de arranque.

### 2.3. Clave AES

Cada repositorio necesita su propia copia local de los mismos 32 bytes en
`keys/client.key`, con permisos `0600`. La clave se provisionó por SSH y se verificó
sin imprimir su contenido; en Melissa se copió con autorización del usuario.
No ejecutar `make keygen` de forma independiente en los remotos.

Desde el **HOST**, esta comparación verifica la copia de Melissa:

```sh
cd /home/jian/Desktop/distributed-typist
ssh -o BatchMode=yes melissa@172.18.100.57 \
    'cmp -s /home/melissa/Documents/sistemasOperativos/proyecto2/distributed-typist/keys/client.key - && stat -c "SHARED_KEY_OK: size=%s mode=%a" /home/melissa/Documents/sistemasOperativos/proyecto2/distributed-typist/keys/client.key' \
    < keys/client.key
```

La salida esperada es `SHARED_KEY_OK: size=32 mode=600`. Para una instalación nueva,
la sección 4 de la guía de dos equipos explica cómo copiarla sin sobrescribir una
existente; adaptar el usuario y la ruta de destino a los de la tabla.

### 2.4. Compilar los ejecutables

En el **HOST** y en el remoto de **`user`**, desde la raíz de cada repositorio:

```sh
typistMpicc="$PWD/build/openmpi-4.1.6/bin/mpicc"
```

En **Melissa**, desde la raíz de su repositorio:

```sh
typistMpicc="/opt/openmpi-4.1.6/bin/mpicc"
```

Después, en **cada equipo y en la misma terminal donde definiste `typistMpicc`**:

```sh
(
set -e
mkdir -p build/three-host
"$typistMpicc" -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -Icommon \
    -std=c11 -O2 -Wall -Wextra -Wpedantic \
    tests/mpiSmoke.c -o build/three-host/mpiSmoke
"$typistMpicc" -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -Icommon \
    -std=c11 -O2 -Wall -Wextra -Wpedantic \
    server/cluster.c server/server.c server/wordCount.c \
    common/protocol.c common/crypto.c -lcrypto -o build/three-host/clusterServer
)
```

En el **HOST**, compilar además el cliente:

```sh
make bin/client
```

Los ejecutables quedan separados de `bin/` y de los usados en la prueba de dos equipos.

### 2.5. Enlace de arranque en cada remoto

Las instalaciones MPI tienen rutas diferentes. Para permitir un único argumento
`--launch-agent`, cada remoto dispone del enlace `$HOME/.local/libexec/typist/orted`:

| Equipo | Destino del enlace |
| --- | --- |
| `user` | `/home/user/distributed-typist/build/openmpi-4.1.6/bin/orted` |
| `melissa` | `/opt/openmpi-4.1.6/bin/orted` |

Para crearlo **una sola vez**, ejecutar en el remoto de **`user`**:

```sh
mkdir -p "$HOME/.local/libexec/typist"
ln -s /home/user/distributed-typist/build/openmpi-4.1.6/bin/orted \
    "$HOME/.local/libexec/typist/orted"
```

Y en **Melissa**:

```sh
mkdir -p "$HOME/.local/libexec/typist"
ln -s /opt/openmpi-4.1.6/bin/orted "$HOME/.local/libexec/typist/orted"
```

Si el enlace ya existe, no volver a crearlo. Consultar su destino en cada remoto
con `readlink -f "$HOME/.local/libexec/typist/orted"`. Estos enlaces no modifican
OpenMPI, no necesitan `sudo` y permanecen después de reiniciar.

## 3. Configuración de lanzamiento en el HOST

Los siguientes archivos **ya están preparados** en `build/three-host/` del host.
No contienen claves; el directorio está excluido de Git. Si se borra `build/`,
recrearlos después de reconstruir los ejecutables.

Desde la raíz del repositorio del **HOST**, crear el inventario:

```sh
mkdir -p build/three-host
cat > build/three-host/hosts <<'EOF'
172.18.37.0 slots=1
172.18.238.179 slots=1
172.18.100.57 slots=1
EOF
```

Crear la configuración que selecciona el usuario SSH para cada remoto:

```sh
cat > build/three-host/ssh-config <<'EOF'
Host 172.18.238.179
    User user

Host 172.18.100.57
    User melissa

Host *
    BatchMode yes
    ConnectTimeout 5
    StrictHostKeyChecking yes
EOF
```

Esta configuración se utiliza únicamente cuando se pasa con `ssh -F`; no sustituye
`~/.ssh/config`. Las claves públicas de los equipos ya deben estar aceptadas en el
archivo habitual `known_hosts` del host. Las claves privadas SSH permanecen en el host.

`--mca plm_rsh_no_tree_spawn 1` hace que el host arranque directamente los demonios
de ambos remotos, usando sus usuarios respectivos. Para este lanzamiento no se
necesita configurar SSH entre los dos remotos; sí necesitan conectividad para MPI.

## 4. Diagnóstico de los tres equipos

Ejecutar **solo en el HOST**, con ambos remotos encendidos y conectados:

```sh
cd /home/jian/Desktop/distributed-typist
build/openmpi-4.1.6/bin/mpirun \
    --noprefix --hostfile build/three-host/hosts --bind-to none --tag-output \
    --launch-agent .local/libexec/typist/orted \
    --mca plm_rsh_agent 'ssh -F /home/jian/Desktop/distributed-typist/build/three-host/ssh-config' \
    --mca plm_rsh_no_tree_spawn 1 \
    --mca pml ob1 --mca btl self,vader,tcp \
    -np 1 --host 172.18.37.0 --wdir "$PWD" \
    "$PWD/build/three-host/mpiSmoke" \
    : -np 1 --host 172.18.238.179 --wdir /home/user/distributed-typist \
    /home/user/distributed-typist/build/three-host/mpiSmoke \
    : -np 1 --host 172.18.100.57 \
    --wdir /home/melissa/Documents/sistemasOperativos/proyecto2/distributed-typist \
    /home/melissa/Documents/sistemasOperativos/proyecto2/distributed-typist/build/three-host/mpiSmoke
```

La prueba debe mostrar `total=6` en los tres procesos, un hostname diferente por rango
y `PASS: all three MPI processes participated (1 + 2 + 3 = 6)`. Este diagnóstico
intercambia números sintéticos; todavía no ejecuta el cliente ni el servidor.

## 5. Servidor distribuido — HOST, terminal 1

```sh
cd /home/jian/Desktop/distributed-typist
mkdir -p output
build/openmpi-4.1.6/bin/mpirun \
    --noprefix --hostfile build/three-host/hosts --bind-to none --tag-output \
    --launch-agent .local/libexec/typist/orted \
    --mca plm_rsh_agent 'ssh -F /home/jian/Desktop/distributed-typist/build/three-host/ssh-config' \
    --mca plm_rsh_no_tree_spawn 1 \
    --mca pml ob1 --mca btl self,vader,tcp \
    -np 1 --host 172.18.37.0 --wdir "$PWD" \
    "$PWD/build/three-host/clusterServer" 127.0.0.1 5000 \
    "$PWD/output/cifrado.bin" "$PWD/output/descifrado.txt" \
    "$PWD/output/conteos.txt" "$PWD/keys/client.key" \
    : -np 1 --host 172.18.238.179 --wdir /home/user/distributed-typist \
    /home/user/distributed-typist/build/three-host/clusterServer 127.0.0.1 5000 \
    output/cifrado.bin output/descifrado.txt output/conteos.txt \
    /home/user/distributed-typist/keys/client.key \
    : -np 1 --host 172.18.100.57 \
    --wdir /home/melissa/Documents/sistemasOperativos/proyecto2/distributed-typist \
    /home/melissa/Documents/sistemasOperativos/proyecto2/distributed-typist/build/three-host/clusterServer \
    127.0.0.1 5000 output/cifrado.bin output/descifrado.txt output/conteos.txt \
    /home/melissa/Documents/sistemasOperativos/proyecto2/distributed-typist/keys/client.key
```

Esperar `INFO: listening on 127.0.0.1:5000` y mantener esta terminal abierta.
No iniciar otro servidor en ese puerto. Los remotos no deben ejecutar su propio
`mpirun`: el host inicia y detiene sus trabajadores.

## 6. Cliente y comprobación — HOST, terminal 2

```sh
cd /home/jian/Desktop/distributed-typist
make client
```

En el indicador `client>`:

```text
enviar nodos.txt
```

El archivo `input/nodos.txt`, ya preparado en el host, contiene:

```text
aaa aaa comun comun
bbb bbb comun comun
ccc ccc comun comun
```

Si no existe, crearlo antes de enviar siguiendo la sección 8.2 de
[mpi-two-hosts.md](mpi-two-hosts.md#82-terminal-2--host-preparar-el-archivo-y-abrir-el-cliente).

La salida debe incluir los tres hostnames y el resultado:

```text
worker rank=0 host=jian-Victus ... bytes=20 words=4 unique=2
worker rank=1 host=administradorce-OptiPlex-7000 ... bytes=20 words=4 unique=2
worker rank=2 host=meliubuntu ... bytes=20 words=4 unique=2
cluster result word=comun count=6 totalWords=12
```

Los PID y el orden pueden variar. Salir del cliente con `salir` y comprobar:

```sh
cmp input/nodos.txt output/descifrado.txt
cat output/conteos.txt
```

Las salidas persistentes se guardan solo en el host y se reemplazan con cada envío
correcto. La prueba automática de preparación utilizó salidas temporales, conservando
los archivos habituales de `output/`.

## 7. Comprobar trabajadores y detener

Desde otra terminal del **HOST**, mientras el servidor está activo:

```sh
ssh -o BatchMode=yes user@172.18.238.179 'pgrep -a -x clusterServer'
ssh -o BatchMode=yes melissa@172.18.100.57 'pgrep -a -x clusterServer'
```

Se espera un trabajador en cada remoto. Las líneas `worker rank=...` después de
un envío confirman que esos procesos participaron en el procesamiento.

Para detener de forma coordinada, usar en el host el PID mostrado al iniciar como
`cluster coordinator pid=...`:

```sh
kill -TERM PID
```

Sustituir `PID` por el número de esa ejecución. Después de la parada ya no deben
aparecer sus trabajadores remotos. En cada remoto se puede observar el consumo
con `top -u user` o `top -u melissa`, según el equipo.

Después de reiniciar no se reinstala nada: comprobar IP y SSH, iniciar el servidor
de la sección 5 y abrir el cliente de la sección 6. Si cambió el código, recompilar
en los tres equipos antes de iniciar.

## 8. Validación y límites

Pasaron el diagnóstico colectivo y el flujo con cliente real e interactivo, un
trabajador por computadora, texto recuperado idéntico, autenticación del cifrado
almacenado y conteos globales exactos. También pasaron archivos de varios bloques
MPI con 3001 palabras únicas, puntuación, UTF-8, archivo vacío y parada coordinada.

No se ha medido aceleración ni rendimiento. Se mantienen los pendientes de seguridad
de [mpi-testing.md](mpi-testing.md#5-pendientes-respecto-al-enunciado-completo), incluido
el cifrado de los campos de protocolo que siguen en claro. SSH no cifra por sí mismo
el tráfico MPI. La Pico H, el driver, la biblioteca y la pantalla siguen fuera de
esta prueba; su integración física permanece pendiente.

Las opciones generales de lanzamiento se describen en el
[manual oficial de mpirun de la serie 4.1](https://www.open-mpi.org/doc/v4.1/man1/mpirun.1.php).
