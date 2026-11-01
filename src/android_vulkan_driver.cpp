// Custom Vulkan driver on Android (for example Mesa Turnip on Adreno).
//
// The launcher copies an imported driver (.so) into the app's files folder and
// passes its path as --vulkan_custom_driver. libadrenotools then loads the
// system libvulkan.so in a private linker namespace whose driver lookup is
// redirected to that file, and the SDK takes the resulting handle in place of
// the system library. Any failure falls back to the system driver.

#include <rex/cvar.h>
#include <rex/logging/macros.h>

#include <adrenotools/driver.h>

#include <dlfcn.h>
#include <sys/stat.h>

#include <filesystem>
#include <mutex>
#include <string>

REXCVAR_DEFINE_STRING(vulkan_custom_driver, "", "Graphics",
                      "Android: path to a custom Vulkan driver .so (e.g. Turnip) to load with "
                      "libadrenotools; empty uses the system driver")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// From rex/ui/vulkan/instance.h, which needs headers the game doesn't include.
namespace rex::ui::vulkan {
using AndroidVulkanLoaderOverride = void* (*)();
void SetAndroidVulkanLoaderOverride(AndroidVulkanLoaderOverride override_function);
}  // namespace rex::ui::vulkan

namespace {

// The folder holding libmain.so and the adrenotools hook libraries.
std::string NativeLibraryDir() {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(&NativeLibraryDir), &info) || !info.dli_fname) return {};
  return std::filesystem::path(info.dli_fname).parent_path().string() + "/";
}

void* OpenCustomDriver() {
  static std::once_flag once;
  static void* handle = nullptr;
  std::call_once(once, [] {
    const std::string driver = REXCVAR_GET(vulkan_custom_driver);
    if (driver.empty()) return;
    std::filesystem::path path(driver);
    struct stat st{};
    if (stat(driver.c_str(), &st) != 0) {
      REXLOG_ERROR("Vulkan driver: {} not found, using the system driver", driver);
      return;
    }
    const std::string hook_dir = NativeLibraryDir();
    const std::string driver_dir = path.parent_path().string() + "/";
    const std::string driver_name = path.filename().string();
    handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr,
                                        hook_dir.c_str(), driver_dir.c_str(),
                                        driver_name.c_str(), nullptr, nullptr);
    if (handle) {
      REXLOG_INFO("Vulkan driver: loaded {} with libadrenotools", driver);
    } else {
      REXLOG_ERROR("Vulkan driver: libadrenotools could not load {} (hooks in {}), using the "
                   "system driver",
                   driver, hook_dir);
    }
  });
  return handle;
}

// Registered when libmain.so loads, before SDL_main parses the cvars; the
// driver is opened later, when the GPU backend creates its Vulkan instance.
const bool registered = [] {
  rex::ui::vulkan::SetAndroidVulkanLoaderOverride(&OpenCustomDriver);
  return true;
}();

}  // namespace
