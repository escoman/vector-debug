// Dummy binary resources for headless testing
// These provide the symbols that board.cpp and icon.cpp expect

extern "C" {
    // Boot ROM dummy data (empty for tests)
    unsigned char _binary_boots_bin_start[1] = {0};
    unsigned char _binary_boots_bin_end[1] = {0};
    unsigned int _binary_boots_bin_size = 0;
    
    // Icon dummy data (empty for tests)
    unsigned char _binary_icon64_png_start[1] = {0};
    unsigned char _binary_icon64_png_end[1] = {0};
    unsigned int _binary_icon64_png_size = 0;
}
