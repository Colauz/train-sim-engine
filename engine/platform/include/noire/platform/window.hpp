#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>  // pour VkInstance / VkSurfaceKHR (intégration Vulkan)

struct GLFWwindow;  // forward-declare : aucun en-tête GLFW dans l'API publique

namespace noire::platform {

struct WindowConfig {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::string title = "Noire Engine";
};

struct FramebufferSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct CursorDelta {
    double dx = 0.0;
    double dy = 0.0;
};

// Touches abstraites (indépendantes de GLFW). AZERTY et QWERTY sont tous deux
// prévus : le mapping ZQSD / WASD se fait côté appelant en combinant les touches.
enum class Key {
    W, A, S, D,      // QWERTY
    Z, Q,            // AZERTY (Z=avancer, Q=gauche)
    Space, LeftShift, LeftControl,
    E, M, H, P,      // E=urgence, M=pluie (M14), H=sifflet, P=portes (M21)
    R, L,            // R=pluie (M21, P reprise par les portes), L=phares (M21)
    Escape,
    Up, Down, Left, Right,
};

// Fenêtre applicative au-dessus de GLFW. La dépendance GLFW est entièrement
// encapsulée : l'appelant ne manipule que des types standard et Vulkan.
class Window {
public:
    explicit Window(WindowConfig config = {});
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool initialize();
    void shutdown();

    [[nodiscard]] bool should_close() const;
    void request_close();
    void poll_events();
    void wait_events();  // bloque (utile quand la fenêtre est minimisée)

    [[nodiscard]] FramebufferSize framebuffer_size() const;
    [[nodiscard]] bool was_resized() const { return resized_; }
    void reset_resized() { resized_ = false; }

    // --- Inputs ---------------------------------------------------------------
    [[nodiscard]] bool is_key_down(Key key) const;
    // Déplacement du curseur depuis le dernier appel (puis remise à zéro).
    [[nodiscard]] CursorDelta consume_cursor_delta();
    void set_cursor_captured(bool captured);  // mode FPS (curseur masqué + illimité)

    // --- Intégration Vulkan (implémentée via GLFW, masquée à l'appelant) ---
    [[nodiscard]] std::vector<const char*> required_instance_extensions() const;
    [[nodiscard]] VkSurfaceKHR create_surface(VkInstance instance) const;

    [[nodiscard]] GLFWwindow* native_handle() const { return window_; }

private:
    static void framebuffer_resize_callback(GLFWwindow* window, int width, int height);

    WindowConfig config_;
    GLFWwindow* window_ = nullptr;
    bool resized_ = false;

    // Suivi du curseur pour calculer les deltas.
    double last_cursor_x_ = 0.0;
    double last_cursor_y_ = 0.0;
    bool cursor_initialized_ = false;
};

}  // namespace noire::platform
