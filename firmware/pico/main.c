#include "pico/stdlib.h"
#include "statusProtocol.h"
#include "tusb.h"

/**
 * @brief Atiende la consulta de estado por EP0 sin ejecutar acciones físicas.
 * @param rhport Puerto raíz USB entregado por TinyUSB.
 * @param stage Etapa de la transferencia de control.
 * @param request Petición válida entregada por TinyUSB; no se conserva su puntero.
 * @return true si acepta la etapa, false para provocar STALL ante peticiones desconocidas.
 */
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                               const tusb_control_request_t *request) {
    static TypistStatusFrame response;

    // La respuesta estática sigue existiendo durante todas las etapas de la transferencia USB.
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (!picoStatusReply(request->bmRequestType, request->bRequest, request->wValue,
                          request->wIndex, request->wLength, &response)) {
        return false;
    }
    return tud_control_xfer(rhport, request, response.bytes, sizeof(response.bytes));
}

/**
 * @brief Inicializa USB en RP2040 y atiende su bucle cooperativo, sin configurar GPIO de servos.
 * @return Nunca retorna durante el funcionamiento normal.
 */
int main(void) {
    // El runtime del Pico SDK inicializa los relojes; no se inicializan UART, LED ni servos.
    if (!tusb_init()) {
        panic("USB initialization failed");
    }
    for (;;) {
        tud_task();
        tight_loop_contents();
    }
}
