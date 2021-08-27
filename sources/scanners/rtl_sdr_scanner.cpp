#include "rtl_sdr_scanner.h"

#include <rtl-sdr.h>
#include <scanners/recorder.h>
#include <spdlog/spdlog.h>
#include <utils.h>

using StreamCallbackData = std::tuple<RtlSdrScanner*, Recorder*, const ConfigFrequencyRange*>;

RtlSdrScanner::RtlSdrScanner(int deviceIndex, std::optional<int> gain, const std::vector<ConfigFrequencyRange>& configFrequencies, const std::vector<ConfigFrequencyRange>& ignoredConfigFrequencies)
    : m_deviceIndex(deviceIndex), m_isRunning(true) {
  char serial[256];
  rtlsdr_get_device_usb_strings(m_deviceIndex, nullptr, nullptr, serial);
  spdlog::info("open rtl sdr device, index: {}, name: {}, serial: {}", m_deviceIndex, rtlsdr_get_device_name(m_deviceIndex), serial);

  if (rtlsdr_open(&m_device, deviceIndex) != 0) {
    throw std::runtime_error("can not open rtl sdr device");
  }

  if (gain.has_value()) {
    if (rtlsdr_set_tuner_gain_mode(m_device, 1) != 0) {
      throw std::runtime_error("can not set tuner gain manual");
    }
    if (rtlsdr_set_tuner_gain(m_device, gain.value()) != 0) {
      throw std::runtime_error("can not set tuner gain");
    }
  } else {
    if (rtlsdr_set_tuner_gain_mode(m_device, 0) != 0) {
      throw std::runtime_error("can not set tuner gain auto");
    }
  }

  if (RTL_SDR_PPM != 0 && rtlsdr_set_freq_correction(m_device, RTL_SDR_PPM) != 0) {
    throw std::runtime_error("can not set tuner ppm");
  }

  uint32_t maxSamples = 0;

  for (const auto& frequencyRange : configFrequencies) {
    maxSamples = std::max(maxSamples, getSamplesCount(frequencyRange.bandwidth(), RANGE_SCANNING_TIME));
    const auto SpectrogramSize = frequencyRange.SpectrogramSize();
    if (m_spectrogram.count(SpectrogramSize) == 0) {
      spdlog::info("create Spectrogram with size: {}", SpectrogramSize);
      m_spectrogram[SpectrogramSize] = std::make_unique<Spectrogram>(SpectrogramSize);
    }
  }
  m_rawBuffer.resize(maxSamples);
  m_buffer.resize(maxSamples / 2);

  m_thread = std::make_unique<std::thread>([this, &configFrequencies]() {
    while (m_isRunning) {
      for (const auto& frequencyRange : configFrequencies) {
        readSamples(frequencyRange);
      }
    }
  });
}

RtlSdrScanner::~RtlSdrScanner() {
  char serial[256];
  rtlsdr_get_device_usb_strings(m_deviceIndex, nullptr, nullptr, serial);
  spdlog::info("close rtl sdr device, index: {}, name: {}, serial: {}", m_deviceIndex, rtlsdr_get_device_name(m_deviceIndex), serial);

  m_isRunning = false;
  m_thread->join();
  rtlsdr_close(m_device);
}

void RtlSdrScanner::readSamples(const ConfigFrequencyRange& frequencyRange) {
  std::unique_lock<std::mutex> lock(m_mutex);

  const auto centerFrequency = frequencyRange.center();
  const auto bandwidth = frequencyRange.bandwidth();
  const auto sampleRate = bandwidth;
  const auto samples = getSamplesCount(sampleRate, RANGE_SCANNING_TIME);
  const auto SpectrogramSize = frequencyRange.SpectrogramSize();

  if (m_lastBandwidth != bandwidth) {
    spdlog::debug("set bandwidth: {} Hz", bandwidth);
    if (rtlsdr_set_tuner_bandwidth(m_device, bandwidth) != 0) {
      throw std::runtime_error("set bandwidth error");
    }
    m_lastBandwidth = bandwidth;
  }
  if (rtlsdr_get_sample_rate(m_device) != sampleRate) {
    spdlog::debug("set sample rate: {} Hz", sampleRate);
    if (rtlsdr_set_sample_rate(m_device, sampleRate) != 0) {
      throw std::runtime_error("set sample rate error");
    }
  }
  if (rtlsdr_get_center_freq(m_device) != centerFrequency) {
    spdlog::debug("set center frequency: {} Hz", centerFrequency);
    if (rtlsdr_set_center_freq(m_device, centerFrequency) != 0) {
      throw std::runtime_error("set center frequency error");
    }
    rtlsdr_reset_buffer(m_device);
  }

  int read{0};
  const auto status = rtlsdr_read_sync(m_device, m_rawBuffer.data(), samples, &read);
  if (status != 0) {
    spdlog::warn("read error: {}", status);
  } else if (read != samples) {
    spdlog::warn("read error, dropped samples: {}", samples - read);
  } else {
    unsigned_to_complex(m_rawBuffer.data(), m_buffer, samples / 2);
    spdlog::debug("read bytes: {}", samples);
    const auto& allSignals = m_spectrogram[SpectrogramSize]->psd(centerFrequency, bandwidth, m_buffer, samples / 2);
    const auto signals = filterSignals(allSignals, frequencyRange);
    const auto maxSignal = selectMaxSignal(signals);
    spdlog::debug("max signal, {}", maxSignal.toString());
    const auto bestSignal = detectBestSignal(signals);
    if (bestSignal.has_value()) {
      spdlog::info("detected signal, {}", bestSignal->toString());
      auto f = [](uint8_t* buf, uint32_t len, void* ctx) {
        spdlog::debug("read bytes: {}", len);
        StreamCallbackData* data = reinterpret_cast<StreamCallbackData*>(ctx);
        RtlSdrScanner* scanner = std::get<0>(*data);
        Recorder* recorder = std::get<1>(*data);
        const ConfigFrequencyRange* frequencyRange = std::get<2>(*data);

        unsigned_to_complex(buf, scanner->m_buffer, len / 2);
        const auto& allSignals = scanner->m_spectrogram[frequencyRange->SpectrogramSize()]->psd(frequencyRange->center(), frequencyRange->bandwidth(), scanner->m_buffer, len / 2);
        const auto signals = filterSignals(allSignals, *frequencyRange);
        const auto maxSignal = selectMaxSignal(signals);
        spdlog::debug("max signal, {}", maxSignal.toString());
        const auto bestSignal = detectBestSignal(signals);
        recorder->appendSamples(maxSignal, bestSignal.has_value(), scanner->m_buffer, len / 2);
        if (recorder->isFinished() || !scanner->m_isRunning) {
          spdlog::info("stop stream");
          rtlsdr_cancel_async(scanner->m_device);
        }
      };
      spdlog::info("start stream");
      Recorder recorder(bestSignal.value(), centerFrequency, bandwidth, sampleRate, *m_spectrogram[SpectrogramSize]);
      StreamCallbackData data({this, &recorder, &frequencyRange});
      rtlsdr_read_async(m_device, f, &data, 0, samples);
    }
  }
}

int RtlSdrScanner::devicesCount() { return rtlsdr_get_device_count(); }
