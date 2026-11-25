#ifndef TYPIST_IOCTL_H
#define TYPIST_IOCTL_H

#include <linux/ioctl.h>
#include "typistUsbProtocol.h"

// La respuesta solo contiene bytes: el ABI no depende del tamaño de punteros o de long.
#define TYPIST_IOCTL_STATUS _IOR('T', 0x01, TypistStatusFrame)

#endif
