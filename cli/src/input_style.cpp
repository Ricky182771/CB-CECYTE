#include "input_style.h"

#include <utility>

namespace chatbot::cli {

ftxui::Element input_transform(ftxui::Element element, bool /*hovered*/, bool /*focused*/,
                               bool is_placeholder, const Palette& palette) {
    if (is_placeholder) {
        return std::move(element) | palette.ink(&Theme::input_placeholder);
    }
    return element;
}

} // namespace chatbot::cli
