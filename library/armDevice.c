#include "armDevice.h"
#include "typistIoctl.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

int armOpen(const char *devicePath) {
    struct stat deviceInfo;
    int deviceFd = -1;
    int metadataResult = -1;

    if (devicePath == NULL || *devicePath == '\0') {
        errno = EINVAL;
        return -1;
    }
    // O_NONBLOCK evita bloquear si se entrega una FIFO por error antes de comprobar el tipo.
    deviceFd = open(devicePath, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (deviceFd == -1) {
        return -1;
    }
    metadataResult = fstat(deviceFd, &deviceInfo);
    if (metadataResult == -1 || !S_ISCHR(deviceInfo.st_mode)) {
        int savedError = metadataResult == -1 ? errno : ENODEV;
        // La ruta inválida no debe dejar un descriptor adquirido ni ocultar el error principal.
        (void)close(deviceFd);
        errno = savedError;
        return -1;
    }
    return deviceFd;
}

int armGetStatus(int deviceFd, ArmStatus *status) {
    TypistStatusFrame frame = {{0}};
    int result = -1;

    if (deviceFd < 0 || status == NULL) {
        errno = EINVAL;
        return -1;
    }
    // Solo se reintenta una consulta sin efectos físicos; no hay operaciones de servo.
    do {
        result = ioctl(deviceFd, TYPIST_IOCTL_STATUS, &frame);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        return -1;
    }
    if (result != 0 || !typistStatusIsValid(&frame)) {
        errno = EPROTO;
        return -1;
    }
    // Publica el estado únicamente después de validar la respuesta completa.
    status->protocolVersion = frame.bytes[4];
    status->state = frame.bytes[5];
    status->capabilities = frame.bytes[6];
    return 0;
}

int armClose(int deviceFd) {
    if (deviceFd < 0) {
        errno = EINVAL;
        return -1;
    }
    return close(deviceFd);
}
