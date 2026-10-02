#include "chatbot/error.h"

namespace chatbot {

const char* error_kind_label(ErrorKind kind) {
    switch (kind) {
    case ErrorKind::Config:
        return "error de configuración";
    case ErrorKind::Auth:
        return "error de autenticación";
    case ErrorKind::ModelNotFound:
        return "modelo no encontrado";
    case ErrorKind::RateLimited:
        return "límite de peticiones alcanzado";
    case ErrorKind::Server:
        return "error del servidor";
    case ErrorKind::Network:
        return "error de red";
    case ErrorKind::Timeout:
        return "tiempo de espera agotado";
    case ErrorKind::BadResponse:
        return "respuesta inválida";
    case ErrorKind::Cancelled:
        return "cancelado";
    }
    return "error desconocido";
}

} // namespace chatbot
