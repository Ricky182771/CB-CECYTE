#include "protocol_stdio.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace chatbot::rpc {

int take_protocol_stdout() {
    // Si el cliente se murió, escribir falla con EPIPE en lugar de matar el proceso.
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return -1;
    }
    const int protocol = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (protocol < 0) {
        return -1;
    }
    if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        close(protocol);
        return -1;
    }
    return protocol;
}

long read_stdin(char* buffer, std::size_t size) {
    while (true) {
        const ssize_t got = read(STDIN_FILENO, buffer, size);
        if (got >= 0 || errno != EINTR) {
            return static_cast<long>(got);
        }
    }
}

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t written = write(fd, data.data(), data.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

} // namespace chatbot::rpc
