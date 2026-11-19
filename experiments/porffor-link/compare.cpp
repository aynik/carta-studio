#include <iostream>
extern "C" int carta_experiment_quantize(double value, int scale, int bits);
int main() {
    double value;
    int scale, bits;
    while (std::cin >> value >> scale >> bits)
        std::cout << carta_experiment_quantize(value, scale, bits) << '\n';
    return std::cin.eof() ? 0 : 1;
}
