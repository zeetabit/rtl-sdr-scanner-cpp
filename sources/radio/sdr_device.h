#pragma once

#include <radio/help_structures.h>

#include <cstdint>
#include <functional>

class SdrDevice {
 public:
  using Callback = std::function<bool(uint8_t*, uint32_t)>;

  virtual void startStream(const FrequencyRange& frequencyRange, Callback&& callback) = 0;
  virtual std::vector<uint8_t> readData(const FrequencyRange& frequencyRange) = 0;
};