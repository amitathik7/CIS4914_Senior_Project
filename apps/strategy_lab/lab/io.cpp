#include "lab/io.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace trading_engine::lab {

void set_binary_mode(std::FILE* stream) noexcept {
#if defined(_WIN32)
    (void)_setmode(_fileno(stream), _O_BINARY);
#else
    (void)stream;
#endif
}

bool write_all(std::FILE* stream, std::string_view bytes) noexcept {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const std::size_t count = std::fwrite(bytes.data() + written, 1, bytes.size() - written, stream);
        if (count == 0) {
            return false;
        }
        written += count;
    }
    return std::fflush(stream) == 0;
}

}  // namespace trading_engine::lab
