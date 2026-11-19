// Runs a fuzz target over every file of a folder of saved inputs. A crash or an
// abort in the target fails the test; so does a folder with nothing in it.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s FOLDER_OF_INPUTS\n", argv[0]);
        return 2;
    }
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(argv[1], error)) {
        if (entry.is_regular_file()) files.push_back(entry.path());
    }
    if (error || files.empty()) {
        std::fprintf(stderr, "no saved inputs in %s\n", argv[1]);
        return 1;
    }
    std::ranges::sort(files);
    for (const auto& file : files) {
        std::ifstream stream(file, std::ios::binary);
        const std::vector<char> content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
    }
    std::fprintf(stderr, "replayed %zu saved inputs\n", files.size());
    return 0;
}
