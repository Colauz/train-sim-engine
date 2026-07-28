#include "noire/platform/window.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "noire/core/log.hpp"

namespace noire::platform {

namespace {
// Traduction Key abstraite -> code GLFW (confine GLFW dans ce fichier).
int to_glfw_key(Key key) {
    switch (key) {
        case Key::W:            return GLFW_KEY_W;
        case Key::A:            return GLFW_KEY_A;
        case Key::S:            return GLFW_KEY_S;
        case Key::D:            return GLFW_KEY_D;
        case Key::Z:            return GLFW_KEY_Z;
        case Key::Q:            return GLFW_KEY_Q;
        case Key::Space:        return GLFW_KEY_SPACE;
        case Key::LeftShift:    return GLFW_KEY_LEFT_SHIFT;
        case Key::LeftControl:  return GLFW_KEY_LEFT_CONTROL;
        case Key::E:            return GLFW_KEY_E;
        case Key::M:            return GLFW_KEY_M;
        case Key::H:            return GLFW_KEY_H;
        case Key::K:            return GLFW_KEY_K;
        case Key::P:            return GLFW_KEY_P;
        case Key::C:            return GLFW_KEY_C;
        case Key::R:            return GLFW_KEY_R;
        case Key::L:            return GLFW_KEY_L;
        case Key::Escape:       return GLFW_KEY_ESCAPE;
        case Key::F11:          return GLFW_KEY_F11;
        case Key::Enter:        return GLFW_KEY_ENTER;
        case Key::Num1:         return GLFW_KEY_1;
        case Key::Num2:         return GLFW_KEY_2;
        case Key::Num3:         return GLFW_KEY_3;
        case Key::Up:           return GLFW_KEY_UP;
        case Key::Down:         return GLFW_KEY_DOWN;
        case Key::Left:         return GLFW_KEY_LEFT;
        case Key::Right:        return GLFW_KEY_RIGHT;
    }
    return GLFW_KEY_UNKNOWN;
}
}  // namespace

Window::Window(WindowConfig config)
    : config_(config), saved_w_(static_cast<int>(config.width)), saved_h_(static_cast<int>(config.height)) {}

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

void Window::request_close() {
    if (window_ != nullptr) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
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

void Window::set_fullscreen(bool enable) {
    if (window_ == nullptr) {
        return;
    }
    const bool currently_fullscreen = (glfwGetWindowMonitor(window_) != nullptr);
    if (enable == currently_fullscreen) {
        return;
    }

    if (enable) {
        glfwGetWindowPos(window_, &saved_x_, &saved_y_);
        glfwGetWindowSize(window_, &saved_w_, &saved_h_);

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        if (monitor != nullptr) {
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            if (mode != nullptr) {
                glfwSetWindowMonitor(window_, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
                log::info("Passage en PLEIN ÉCRAN : {}x{}@{}Hz", mode->width, mode->height, mode->refreshRate);
            }
        }
    } else {
        glfwSetWindowMonitor(window_, nullptr, saved_x_, saved_y_, saved_w_, saved_h_, 0);
        log::info("Passage en mode FENÊTRÉ : {}x{}", saved_w_, saved_h_);
    }
    resized_ = true;
}

void Window::toggle_fullscreen() {
    set_fullscreen(!is_fullscreen());
}

bool Window::is_fullscreen() const {
    return window_ != nullptr && (glfwGetWindowMonitor(window_) != nullptr);
}

bool Window::is_key_down(Key key) const {
    if (window_ == nullptr) {
        return false;
    }
    return glfwGetKey(window_, to_glfw_key(key)) == GLFW_PRESS;
}

CursorDelta Window::consume_cursor_delta() {
    if (window_ == nullptr) {
        return {};
    }
    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(window_, &x, &y);

    if (!cursor_initialized_) {  // premier appel : pas de delta parasite
        last_cursor_x_ = x;
        last_cursor_y_ = y;
        cursor_initialized_ = true;
    }
    const CursorDelta delta{x - last_cursor_x_, y - last_cursor_y_};
    last_cursor_x_ = x;
    last_cursor_y_ = y;
    return delta;
}

void Window::set_cursor_captured(bool captured) {
    if (window_ == nullptr) {
        return;
    }
    glfwSetInputMode(window_, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    cursor_initialized_ = false;  // on repartira d'un delta nul après un changement de mode
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
