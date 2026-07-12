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
    void poll_events();
    void wait_events();  // bloque (utile quand la fenêtre est minimisée)

    [[nodiscard]] FramebufferSize framebuffer_size() const;
    [[nodiscard]] bool was_resized() const { return resized_; }
    void reset_resized() { resized_ = false; }

    // --- Intégration Vulkan (implémentée via GLFW, masquée à l'appelant) ---
    [[nodiscard]] std::vector<const char*> required_instance_extensions() const;
    [[nodiscard]] VkSurfaceKHR create_surface(VkInstance instance) const;

    [[nodiscard]] GLFWwindow* native_handle() const { return window_; }

private:
    static void framebuffer_resize_callback(GLFWwindow* window, int width, int height);

    WindowConfig config_;
    GLFWwindow* window_ = nullptr;
    bool resized_ = false;
};

}  // namespace noire::platform
