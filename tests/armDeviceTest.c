#include "armDevice.h"
#include "statusProtocol.h"
#include "typistIoctl.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static int mockEnabled;
static int interruptedOnce;
static int simulatedError;
static unsigned int ioctlCalls;
static TypistStatusFrame simulatedReply;

/**
 * @brief Declara la operación real para comprobar también un dispositivo que no es nuestro módulo.
 * @param fd Descriptor de prueba.
 * @param request Número de ioctl.
 * @param ... Puntero al destino de la respuesta.
 * @return Resultado real de ioctl, con errno ante error.
 */
int __real_ioctl(int fd, unsigned long request, ...);

/**
 * @brief Inyecta respuestas y fallos en la biblioteca sin simular una prueba USB física.
 * @param fd Descriptor recibido de la biblioteca.
 * @param request Comando que debe coincidir con nuestra ABI.
 * @param ... Puntero que recibe TypistStatusFrame.
 * @return 0 con respuesta o -1 con el errno seleccionado.
 */
int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list arguments;
    TypistStatusFrame *response;

    va_start(arguments, request);
    response = va_arg(arguments, TypistStatusFrame *);
    va_end(arguments);
    if (!mockEnabled) {
        return __real_ioctl(fd, request, response);
    }
    assert(request == TYPIST_IOCTL_STATUS);
    ioctlCalls++;
    if (interruptedOnce && ioctlCalls == 1) {
        errno = EINTR;
        return -1;
    }
    if (simulatedError != 0) {
        errno = simulatedError;
        return -1;
    }
    *response = simulatedReply;
    return 0;
}

/**
 * @brief Comprueba la petición exacta que el driver envía y el rechazo de variaciones por firmware.
 */
static void verifyFirmwareProtocol(void) {
    TypistStatusFrame response = {{0}};
    TypistStatusFrame previous;

    assert(sizeof(response) == TYPIST_STATUS_SIZE);
    assert(picoStatusReply(TYPIST_USB_REQUEST_TYPE, TYPIST_USB_REQUEST_STATUS,
                           TYPIST_PROTOCOL_VERSION, TYPIST_USB_INTERFACE,
                           TYPIST_STATUS_SIZE, &response) == 1);
    assert(typistStatusIsValid(&response));
    previous = response;
    // No permite versiones, interfaces, direcciones ni longitudes incompatibles.
    assert(!picoStatusReply(0x41, 1, 1, 0, 8, &response));
    assert(!picoStatusReply(0xc0, 1, 1, 0, 8, &response));
    assert(!picoStatusReply(0xc1, 2, 1, 0, 8, &response));
    assert(!picoStatusReply(0xc1, 1, 2, 0, 8, &response));
    assert(!picoStatusReply(0xc1, 1, 1, 1, 8, &response));
    assert(!picoStatusReply(0xc1, 1, 1, 0, 7, &response));
    assert(!picoStatusReply(0xc1, 1, 1, 0, 9, &response));
    assert(!picoStatusReply(0xc1, 1, 1, 0, 8, NULL));
    assert(memcmp(&previous, &response, sizeof(response)) == 0);
}

/**
 * @brief Verifica errores del dispositivo, consultas interrumpidas y respuestas incompatibles.
 */
static void verifyLibraryQueries(void) {
    ArmStatus status = {0};
    const ArmStatus sentinel = {0xaa, 0xbb, 0xcc};
    const TypistStatusFrame valid = TYPIST_STATUS_INITIALIZER;

    mockEnabled = 1;
    simulatedReply = valid;
    assert(armGetStatus(-1, &status) == -1 && errno == EINVAL);
    assert(armGetStatus(123, NULL) == -1 && errno == EINVAL);
    assert(armGetStatus(123, &status) == 0);
    assert(status.protocolVersion == TYPIST_PROTOCOL_VERSION);
    assert(status.state == TYPIST_STATE_UNCONFIGURED);
    assert(status.capabilities == TYPIST_CAP_STATUS);

    // Una consulta de estado puede reintentarse: no hay efectos físicos que se dupliquen.
    ioctlCalls = 0;
    interruptedOnce = 1;
    assert(armGetStatus(123, &status) == 0 && ioctlCalls == 2);
    interruptedOnce = 0;
    simulatedError = ENODEV;
    status = sentinel;
    assert(armGetStatus(123, &status) == -1 && errno == ENODEV);
    assert(memcmp(&status, &sentinel, sizeof(status)) == 0);
    simulatedError = 0;

    // Cada byte forma parte del contrato: nunca acepta una calibración o capacidad inventadas.
    for (size_t offset = 0; offset < sizeof(valid.bytes); offset++) {
        simulatedReply = valid;
        simulatedReply.bytes[offset] ^= 0x80;
        status = sentinel;
        assert(armGetStatus(123, &status) == -1 && errno == EPROTO);
        assert(memcmp(&status, &sentinel, sizeof(status)) == 0);
    }
    mockEnabled = 0;
}

/**
 * @brief Rechaza archivos, FIFO y rutas inexistentes y comprueba errno de un dispositivo ajeno.
 */
static void verifyDevicePaths(void) {
    char path[] = "/tmp/typist-device-test-XXXXXX";
    int temporaryFd = mkstemp(path);
    int deviceFd;
    ArmStatus status;

    assert(temporaryFd != -1);
    assert(armOpen(NULL) == -1 && errno == EINVAL);
    assert(armOpen("") == -1 && errno == EINVAL);
    assert(armOpen(path) == -1 && errno == ENODEV);
    assert(close(temporaryFd) == 0);
    assert(unlink(path) == 0);
    assert(armOpen(path) == -1 && errno == ENOENT);
    assert(mkfifo(path, 0600) == 0);
    assert(armOpen(path) == -1 && errno == ENODEV);
    assert(unlink(path) == 0);

    // Un nodo de carácter genérico no se confunde con una comunicación correcta del proyecto.
    deviceFd = armOpen("/dev/null");
    assert(deviceFd != -1);
    assert(armGetStatus(deviceFd, &status) == -1 && errno == ENOTTY);
    assert(armClose(deviceFd) == 0);
    assert(armClose(-1) == -1 && errno == EINVAL);
}

/**
 * @brief Ejecuta comprobaciones nativas del contrato y de la biblioteca sin cargar el kernel.
 * @return EXIT_SUCCESS si todas las comprobaciones pasan.
 */
int main(void) {
    verifyFirmwareProtocol();
    verifyLibraryQueries();
    verifyDevicePaths();
    puts("PASS: firmware requests, status validation, ioctl errors and device paths");
    return EXIT_SUCCESS;
}
