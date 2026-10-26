#ifndef CHATBOT_RPC_PROTOCOL_STDIO_H
#define CHATBOT_RPC_PROTOCOL_STDIO_H

#include <cstddef>
#include <string_view>

namespace chatbot::rpc {

// stdin y stdout del protocolo. CMake compila stdio_posix.cpp o
// stdio_windows.cpp.

/// Prepara stdio para el protocolo y devuelve el descriptor (una copia del 1)
/// que solo usa el escritor; el 1 queda apuntando a stderr, así lo que una
/// biblioteca imprima por accidente no rompe el protocolo. En POSIX ignora
/// SIGPIPE y la copia no se hereda a otros procesos; en Windows pone stdin y
/// la copia en modo binario (sin "\n" → "\r\n"). -1 si falló.
[[nodiscard]] int take_protocol_stdout();

/// Lee de stdin lo que haya, hasta size bytes: > 0 leídos, 0 fin, < 0 error.
[[nodiscard]] long read_stdin(char* buffer, std::size_t size);

/// Escribe todo data en fd; false si falló (el cliente se fue).
[[nodiscard]] bool write_all(int fd, std::string_view data);

} // namespace chatbot::rpc

#endif // CHATBOT_RPC_PROTOCOL_STDIO_H
