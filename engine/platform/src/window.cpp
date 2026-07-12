#include "noire/platform/window.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "noire/core/log.hpp"

namespace noire::platform {

Window::Window(WindowConfig config) : config_(config) {}

Window::~Window() { shutdown(); }

bool Window::initialize() {
    if (glfwInit() != GLFW_TRUE) {
        log::error("GLFW : échec de l'initialisation");
        return false;
    }
    if (glfwVulkanSupported() != GLFW_TRUE) {
        log::error("GLFW : Vulkan non supporté sur ce système");
        glfwTerminate();
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // pas de contexte OpenGL : c'est du Vulkan

    window_ = glfwCreateWindow(static_cast<int>(config_.width), static_cast<int>(config_.height),
                               config_.title.c_str(), nullptr, nullptr);
    if (window_ == nullptr) {
        log::error("GLFW : échec de création de la fenêtre");
        glfwTerminate();
        return false;
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, &Window::framebuffer_resize_callback);

    log::info("Fenêtre créée : {}x{} « {} »", config_.width, config_.height, config_.title);
    return true;
}

void Window::shutdown() {
    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
    }
}

bool Window::should_close() const {
    return window_ != nullptr && glfwWindowShouldClose(window_) == GLFW_TRUE;
}

void Window::poll_events() { glfwPollEvents(); }

void Window::wait_events() { glfwWaitEvents(); }

FramebufferSize Window::framebuffer_size() const {
    int w = 0;
    int h = 0;
    if (window_ != nullptr) {
        glfwGetFramebufferSize(window_, &w, &h);
    }
    return FramebufferSize{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
}

std::vector<const char*> Window::required_instance_extensions() const {
    std::uint32_t count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&count);
    return std::vector<const char*>(extensions, extensions + count);
}

VkSurfaceKHR Window::create_surface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(instance, window_, nullptr, &surface) != VK_SUCCESS) {
        log::error("GLFW : échec de création de la surface Vulkan");
        return VK_NULL_HANDLE;
    }
    return surface;
}

void Window::framebuffer_resize_callback(GLFWwindow* window, int /*width*/, int /*height*/) {
    auto* self = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self != nullptr) {
        self->resized_ = true;
    }
}

}  // namespace noire::platform
