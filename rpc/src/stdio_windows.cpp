#include "protocol_stdio.h"

#include <climits>
#include <fcntl.h>
#include <io.h>

namespace chatbot::rpc {

int take_protocol_stdout() {
    const int protocol = _dup(1);
    if (protocol < 0) {
        return -1;
    }
    if (_dup2(2, 1) < 0 || _setmode(0, _O_BINARY) < 0 || _setmode(protocol, _O_BINARY) < 0) {
        _close(protocol);
        return -1;
    }
    return protocol;
}

long read_stdin(char* buffer, std::size_t size) {
    return _read(0, buffer, static_cast<unsigned int>(size > INT_MAX ? INT_MAX : size));
}

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const std::size_t chunk = data.size() > INT_MAX ? INT_MAX : data.size();
        const int written = _write(fd, data.data(), static_cast<unsigned int>(chunk));
        if (written < 0) {
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(written));
    }
    return true;
}

} // namespace chatbot::rpc
