#include "statusProtocol.h"

int picoStatusReply(unsigned char requestType, unsigned char request, unsigned short value,
                    unsigned short index, unsigned short length, TypistStatusFrame *response) {
    const TypistStatusFrame unconfigured = TYPIST_STATUS_INITIALIZER;

    // No acepta transferencias de salida, otras interfaces, longitudes ni versiones implícitas.
    if (response == 0 || requestType != TYPIST_USB_REQUEST_TYPE ||
        request != TYPIST_USB_REQUEST_STATUS || value != TYPIST_PROTOCOL_VERSION ||
        index != TYPIST_USB_INTERFACE || length != TYPIST_STATUS_SIZE) {
        return 0;
    }
    *response = unconfigured;
    return 1;
}
