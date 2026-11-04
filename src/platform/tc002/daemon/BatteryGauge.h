#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace awtrix::tc002d {

// Charge percentage of the TC002 cell from the MCU's battery voltage and USB samples. The MCU's own
// percentage needs 4200 mV for 100 %, but it measures the charger's full voltage at 4100 to 4130 mV,
// and it keeps stale values across USB changes, so it is not used.
//
// The median of the last kWindow samples, at least kSpacingMs apart (about five minutes), goes
// through curves measured on a TC002, so short loads such as a sound do not count. The first sample
// gives a provisional percentage, which the first full window replaces. After a USB change the
// percentage holds until the window is full of new samples. On battery it falls along the discharge
// curve, and rises by one only after a whole window kRiseMargin above it. On USB it only rises,
// along the charge curve up to 100 % at the charger's full voltage. Nothing the MCU reports tells
// how far the charger is past that point, so 100 % comes before the charge light turns green.
//
// The display draws most of the current, and the cell's voltage sags with it. With the display
// load (per mille of a full-white frame at full brightness, Tc002Board), the gauge learns the sag
// per per mille from readings kLearnMinMs to kLearnMaxMs apart whose load differs by kMinLoadStep,
// and on battery moves each reading to kReferenceLoad, the load the curves are taken for.
// The display load as the runtime reports it, counted only once it held for kSteadyMs: the MCU's
// battery reading is a few seconds old, so right after a change it still belongs to the old load.
class LoadTracker {
 public:
  static constexpr int kSteadyStep = 20;          // per mille between reports that count as a change
  static constexpr int64_t kSteadyMs = 8000;
  static constexpr int64_t kStaleMs = 10000;

  void report(int perMille, int64_t nowMs);
  int last(int64_t nowMs) const { return load_ >= 0 && nowMs - reportedAt_ <= kStaleMs ? load_ : -1; }
  // -1 while the load changed within kSteadyMs or no report is fresh.
  int steady(int64_t nowMs) const { return last(nowMs) >= 0 && nowMs - changedAt_ >= kSteadyMs ? load_ : -1; }

 private:
  int load_ = -1;
  int64_t reportedAt_ = 0, changedAt_ = 0;
};

class BatteryGauge {
 public:
  static constexpr std::size_t kWindow = 31;
  static constexpr int64_t kSpacingMs = 9000;  // the daemon queries every 10 s
  static constexpr int kRiseMargin = 3;
  static constexpr int kReferenceLoad = 50;
  static constexpr int kMinLoadStep = 40;
  static constexpr int64_t kLearnMinMs = 5000, kLearnMaxMs = 30000;
  static constexpr double kMaxSag = 3.0;  // mV per per mille; larger estimates are not sag
  static constexpr std::size_t kSagSamples = 9;

  // load: -1 when unknown; the reading then counts as it is.
  void sample(int millivolts, bool usb, int64_t nowMs, int load = -1);
  int percent() const { return percent_; }  // -1 before the first sample
  double sag() const { return sag_; }        // mV per per mille of display load, 0 until learned
  int lastCompensated() const { return compensated_; }

  static int estimate(int millivolts, bool usb);

 private:
  void learn(int millivolts, bool usb, int64_t nowMs, int load);

  std::array<int, kWindow> window_{};
  std::size_t count_ = 0, next_ = 0;
  bool usb_ = false, settled_ = false;
  int percent_ = -1, compensated_ = -1;
  unsigned risen_ = 0;
  int64_t acceptedAt_ = 0;
  std::array<double, kSagSamples> sags_{};
  std::size_t sagCount_ = 0, sagNext_ = 0;
  double sag_ = 0;
  int lastMillivolts_ = -1, lastLoad_ = -1;
  bool lastUsb_ = false;
  int64_t lastAt_ = 0;
};

}
