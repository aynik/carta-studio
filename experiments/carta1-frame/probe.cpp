#include "frame.h"
#include <array>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 16;
    if (frames < 1 || frames > 500000) return 2;
    for (int frame = 0; frame < frames; ++frame) {
        for (int stream = 0; stream < 2; ++stream) {
            std::array<float, 512> input{}, output{};
            std::array<unsigned char, 212> encoded{};
            for (int i = 0; i < 512; ++i) {
                const int n = (frame * 113 + stream * 379 + i * 37) % 2049 - 1024;
                input[static_cast<size_t>(i)] = frame == 0 ? 0.0f :
                    frame % 23 == 1 ? (i == 127 + stream ? 0.875f : 0.0f) :
                    static_cast<float>(n) / 2048.0f;
            }
            if (carta_frame_encode(stream, input.data(), encoded.data()) ||
                carta_frame_decode(stream, encoded.data(), output.data())) return 3;
            if (std::fwrite(encoded.data(), 1, encoded.size(), stdout) != encoded.size() ||
                std::fwrite(output.data(), sizeof(float), output.size(), stdout) != output.size()) return 4;
        }
    }
}
