#include "input_style.h"

#include <utility>

namespace chatbot::cli {

ftxui::Element input_transform(ftxui::Element element, bool /*hovered*/, bool /*focused*/,
                               bool is_placeholder) {
    if (is_placeholder) {
        return ftxui::dim(std::move(element));
    }
    return element;
}

} // namespace chatbot::cli
