#ifndef CHATBOT_CLI_PROVIDERS_H
#define CHATBOT_CLI_PROVIDERS_H

#include <span>
#include <string_view>

namespace chatbot::cli {

/// Un proveedor compatible con la API de OpenAI.
struct ProviderInfo {
    std::string_view id;       ///< Lo que se guarda en config.json ("provider").
    std::string_view name;     ///< Nombre en pantalla.
    std::string_view base_url; ///< Vacía en "custom": la escribe el usuario.
    bool needs_key = true;     ///< false: servidor local (la key es opcional).
};

/// Id del endpoint personalizado (la URL base la escribe el usuario).
inline constexpr std::string_view kCustomProvider = "custom";

/// Tabla de proveedores, en el orden de la pantalla; "custom" va al final.
/// Las URL base se confirmaron en la documentación de cada proveedor
/// (octubre de 2026). Único lugar del código con esta tabla.
[[nodiscard]] std::span<const ProviderInfo> providers();

/// Índice del proveedor con ese id en providers(), o el de "custom" si no
/// existe (o está vacío).
[[nodiscard]] std::size_t provider_index(std::string_view id);

} // namespace chatbot::cli

#endif // CHATBOT_CLI_PROVIDERS_H
