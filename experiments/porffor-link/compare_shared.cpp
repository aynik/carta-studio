#include <iostream>
extern "C" int carta_experiment_quantize(double, int, int);
extern "C" int carta_experiment_quantize_second(double, int, int);
int main() {
    double value;
    int scale, bits;
    while (std::cin >> value >> scale >> bits) {
        auto first = carta_experiment_quantize(value, scale, bits);
        auto second = carta_experiment_quantize_second(value, scale, bits);
        if (first != second) return 2;
        std::cout << first << '\n';
    }
    return std::cin.eof() ? 0 : 1;
}
