/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <android/log.h>
#include <dlfcn.h>
#include <hardware/hardware.h>
#include <hidl/HidlTransportSupport.h>
#include <unistd.h>

#include <cstdio>

namespace {

constexpr char kTag[] = "AERAAudio";
constexpr const char* kPrimaryHalCandidates[] = {
    "/system/aera-audio-vendor/lib64/hw/audio.primary.taro.so",
    "/system/aera-audio-vendor/lib64/hw/audio.primary.ukee.so",
    "/system/aera-audio-vendor/lib64/hw/audio.primary.default.so",
};
constexpr char kAudioHardwareInterface[] = "audio_hw_if";

struct AudioModule {
  hw_module_t common;
};

void Log(int priority, const char* message) {
  __android_log_write(priority, kTag, message);
  std::fprintf(stderr, "%s\n", message);
}

}

int main(int, char**) {
  if (getuid() != 0) {
    Log(ANDROID_LOG_ERROR, "must run as root");
    return 65;
  }
  const char* primary_hal = nullptr;
  for (const char* candidate : kPrimaryHalCandidates) {
    if (access(candidate, R_OK) == 0) {
      primary_hal = candidate;
      break;
    }
  }
  if (!primary_hal) {
    Log(ANDROID_LOG_ERROR, "no stock audio HAL");
    return 66;
  }

  android::hardware::configureRpcThreadpool(8, true);

  void* library = dlopen(primary_hal, RTLD_NOW | RTLD_GLOBAL);
  if (!library) {
    __android_log_print(ANDROID_LOG_ERROR, kTag,
                        "hal load failed: %s", dlerror());
    return 67;
  }

  dlerror();
  auto* module = static_cast<AudioModule*>(
      dlsym(library, HAL_MODULE_INFO_SYM_AS_STR));
  const char* symbol_error = dlerror();
  if (!module || symbol_error || !module->common.methods ||
      !module->common.methods->open) {
    __android_log_print(ANDROID_LOG_ERROR, kTag,
                        "hal has no entry point: %s",
                        symbol_error ? symbol_error : "empty module");
    dlclose(library);
    return 68;
  }

  hw_device_t* raw_device = nullptr;
  const int status = module->common.methods->open(
      &module->common, kAudioHardwareInterface, &raw_device);
  if (status != 0 || !raw_device) {
    __android_log_print(ANDROID_LOG_ERROR, kTag,
                        "hal init failed: %d", status);
    dlclose(library);
    return 69;
  }

  Log(ANDROID_LOG_INFO, "pal/agm up");
  android::hardware::joinRpcThreadpool();

  Log(ANDROID_LOG_ERROR, "hidl thread pool exited");
  raw_device->close(raw_device);
  dlclose(library);
  return 70;
}
