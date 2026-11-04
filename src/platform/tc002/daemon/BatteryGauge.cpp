#include "platform/tc002/daemon/BatteryGauge.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace awtrix::tc002d {

namespace {
struct Point {
  int millivolts, percent;
};

// Measured on a TC002 on 2026-10-09 with the display at full brightness throughout, readings as the
// MCU reports them. A second TC002 stayed within 4 % of the discharge curve.
// On battery: from a full charge until it switched off (6 h 50 min), against the share of the
// runtime left.
constexpr Point kDischarge[] = {
    {4090, 100}, {4070, 95}, {4050, 90}, {4030, 85}, {4010, 78}, {4000, 70}, {3980, 65},
    {3950, 60},  {3920, 55}, {3890, 50}, {3870, 45}, {3850, 40}, {3840, 35}, {3820, 30},
    {3790, 25},  {3760, 20}, {3720, 15}, {3660, 10}, {3590, 6},  {3550, 5},  {3430, 3},
    {3240, 1},   {3100, 0},
};
// On USB: from empty until the charger reached its full voltage (2 h 27 min at constant current),
// against the share of that time.
constexpr Point kCharge[] = {
    {4080, 100}, {4050, 95}, {4030, 90}, {4010, 85}, {3990, 78}, {3970, 68}, {3960, 60},
    {3950, 55},  {3940, 50}, {3930, 45}, {3910, 40}, {3890, 35}, {3870, 30}, {3840, 25},
    {3800, 20},  {3760, 15}, {3670, 10}, {3600, 8},  {3480, 5},  {3270, 2},  {3100, 0},
};

// Interpolates and rounds down, so the top percentage needs the top reading.
template <std::size_t N>
int lookup(const Point (&curve)[N], int millivolts) {
  if (millivolts >= curve[0].millivolts) return curve[0].percent;
  for (std::size_t i = 1; i < N; ++i) {
    const Point& lo = curve[i];
    if (millivolts < lo.millivolts) continue;
    const Point& hi = curve[i - 1];
    return lo.percent + (millivolts - lo.millivolts) * (hi.percent - lo.percent) / (hi.millivolts - lo.millivolts);
  }
  return curve[N - 1].percent;
}
}

int BatteryGauge::estimate(int millivolts, bool usb) {
  return usb ? lookup(kCharge, millivolts) : lookup(kDischarge, millivolts);
}

void LoadTracker::report(int perMille, int64_t nowMs) {
  if (load_ < 0 || std::abs(perMille - load_) >= kSteadyStep || nowMs - reportedAt_ > kStaleMs) changedAt_ = nowMs;
  load_ = perMille;
  reportedAt_ = nowMs;
}

void BatteryGauge::learn(int millivolts, bool usb, int64_t nowMs, int load) {
  const bool paired = lastLoad_ >= 0 && load >= 0 && !usb && !lastUsb_ && nowMs - lastAt_ >= kLearnMinMs &&
                      nowMs - lastAt_ <= kLearnMaxMs && std::abs(load - lastLoad_) >= kMinLoadStep;
  if (paired) {
    const double estimate = static_cast<double>(lastMillivolts_ - millivolts) / (load - lastLoad_);
    // A step against the load is noise on a flat cell; it counts as no sag rather than skewing the median.
    if (estimate <= kMaxSag) {
      sags_[sagNext_] = std::max(0.0, estimate);
      sagNext_ = (sagNext_ + 1) % sags_.size();
      if (sagCount_ < sags_.size()) ++sagCount_;
      if (sagCount_ >= 3) {
        std::array<double, kSagSamples> sorted{};
        std::copy(sags_.begin(), sags_.begin() + static_cast<std::ptrdiff_t>(sagCount_), sorted.begin());
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sagCount_ / 2),
                         sorted.begin() + static_cast<std::ptrdiff_t>(sagCount_));
        sag_ = sorted[sagCount_ / 2];
      }
    }
  }
  if (load >= 0 && (lastLoad_ < 0 || nowMs - lastAt_ >= kLearnMinMs || usb != lastUsb_)) {
    lastMillivolts_ = millivolts;
    lastLoad_ = load;
    lastUsb_ = usb;
    lastAt_ = nowMs;
  }
}

void BatteryGauge::sample(int millivolts, bool usb, int64_t nowMs, int load) {
  if (millivolts < 0) return;
  learn(millivolts, usb, nowMs, load);
  if (!usb && load >= 0) millivolts += static_cast<int>(std::lround(sag_ * (load - kReferenceLoad)));
  compensated_ = millivolts;
  if (percent_ < 0 || usb != usb_) {
    if (percent_ < 0) percent_ = estimate(millivolts, usb);
    usb_ = usb;
    count_ = next_ = 0;
    risen_ = 0;
  } else if (nowMs - acceptedAt_ < kSpacingMs) {
    return;
  }
  acceptedAt_ = nowMs;
  window_[next_] = millivolts;
  next_ = (next_ + 1) % window_.size();
  if (count_ < window_.size()) ++count_;
  if (count_ < window_.size()) return;
  auto sorted = window_;
  std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
  const int now = estimate(sorted[sorted.size() / 2], usb);
  if (!settled_) {
    // The first sample may come under the boot load; the first full window replaces it.
    settled_ = true;
    percent_ = now;
  } else if (usb) {
    percent_ = std::max(percent_, now);
  } else if (now < percent_) {
    percent_ = now;
    risen_ = 0;
  } else if (now >= percent_ + kRiseMargin) {
    if (++risen_ >= window_.size()) {
      ++percent_;
      risen_ = 0;
    }
  } else {
    risen_ = 0;
  }
}

}
