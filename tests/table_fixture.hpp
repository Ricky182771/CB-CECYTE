#ifndef CHATBOT_TEST_TABLE_FIXTURE_HPP
#define CHATBOT_TEST_TABLE_FIXTURE_HPP

#include <string>

namespace chatbot_test {

inline std::string table_10x50() {
    std::string big = "|";
    for (int c = 0; c < 10; ++c) {
        big += " Columna " + std::to_string(c) + " |";
    }
    big += "\n|";
    for (int c = 0; c < 10; ++c) {
        big += "---|";
    }
    big += "\n";
    const std::string words = "texto de relleno con palabras de largo variable para la celda ";
    for (int r = 0; r < 50; ++r) {
        big += "|";
        for (int c = 0; c < 10; ++c) {
            std::string cell;
            const int repeat = 1 + (r * 7 + c * 3) % 6;
            for (int k = 0; k < repeat; ++k) {
                cell += words;
            }
            big += " " + cell + "|";
        }
        big += "\n";
    }
    return big;
}

} // namespace chatbot_test

#endif
