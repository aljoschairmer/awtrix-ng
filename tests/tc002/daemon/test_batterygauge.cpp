// BatteryGauge with voltages as the TC002's MCU reports them and synthetic sample times.
#include "platform/tc002/daemon/BatteryGauge.h"
#include "support.h"

#include <array>
#include <cstdlib>

using awtrix::tc002d::BatteryGauge;
using awtrix::tc002d::LoadTracker;
using tc002d_test::check;

namespace {

constexpr int64_t kStep = 10000;  // the daemon's battery query interval
constexpr int kWindow = static_cast<int>(BatteryGauge::kWindow);

// One reading per query interval; returns the time of the next one.
int64_t feed(BatteryGauge& gauge, int millivolts, bool usb, int count, int64_t now) {
  for (int i = 0; i < count; ++i, now += kStep) gauge.sample(millivolts, usb, now);
  return now;
}

void estimates() {
  check(BatteryGauge::estimate(4100, true) == 100, "charger full voltage is 100 %");
  check(BatteryGauge::estimate(4080, true) == 100, "measured: the charger reached its full voltage at 4080 mV");
  check(BatteryGauge::estimate(4079, true) == 99, "below it the charge is still running");
  check(BatteryGauge::estimate(3940, true) == 50, "measured: half the charging time at 3940 mV");
  check(BatteryGauge::estimate(4089, false) == 99, "rounds down: 100 % needs the top reading");
  check(BatteryGauge::estimate(4090, false) == 100, "a full cell on battery is 100 %");
  check(BatteryGauge::estimate(3890, false) == 50, "measured: half the runtime left at 3890 mV");
  check(BatteryGauge::estimate(3660, false) == 10, "measured: a tenth left at 3660 mV");
  check(BatteryGauge::estimate(3905, false) == 52, "interpolated between measured points");
  check(BatteryGauge::estimate(3900, true) < BatteryGauge::estimate(3900, false),
        "the same reading means less charge while charging");
  check(BatteryGauge::estimate(3000, false) == 0 && BatteryGauge::estimate(3000, true) == 0, "empty clamps to 0 %");
  check(BatteryGauge::estimate(4300, false) == 100, "above full clamps to 100 %");
  int previous = -1;
  bool rising = true;
  for (int mv = 3400; mv <= 4200; mv += 5) {
    const int now = BatteryGauge::estimate(mv, false);
    rising = rising && now >= previous;
    previous = now;
  }
  check(rising, "higher readings never mean less charge");
}

void reportedFullCharge() {
  // The MCU said 72 % here, with USB connected and the charger at its full voltage.
  BatteryGauge gauge;
  check(gauge.percent() == -1, "unknown before the first sample");
  int64_t now = feed(gauge, 4100, true, 1, 0);
  check(gauge.percent() == 100, "first sample at full voltage on USB reads 100 %");
  feed(gauge, 4095, true, 2 * kWindow, now);
  check(gauge.percent() == 100, "ADC steps below full do not drop it");

  // Measured: plugged in on battery at 4000 mV, the reading reached the full voltage at once.
  BatteryGauge plugged;
  now = feed(plugged, 4000, false, kWindow, 0);
  const int before = plugged.percent();
  now = feed(plugged, 4095, true, kWindow - 1, now);
  check(plugged.percent() == before, "holds until the window is full of charging samples");
  feed(plugged, 4095, true, 1, now);
  check(plugged.percent() == 100, "then the charger's full voltage reads 100 %");
}

void charging() {
  BatteryGauge gauge;
  int64_t now = feed(gauge, 3900, true, kWindow, 0);
  const int start = gauge.percent();
  check(start > 0 && start < 100, "a partly charged cell on USB");
  now = feed(gauge, 3850, true, kWindow, now);
  check(gauge.percent() == start, "never falls while USB is connected");
  now = feed(gauge, 4000, true, kWindow, now);
  check(gauge.percent() > start && gauge.percent() < 100, "rises with the charging voltage");
  now = feed(gauge, 4079, true, kWindow, now);
  check(gauge.percent() > start && gauge.percent() < 100, "below the full voltage it never reaches 100 %");
  feed(gauge, 4100, true, kWindow, now);
  check(gauge.percent() == 100, "reaches 100 % at the charger's full voltage");
}

void discharging() {
  BatteryGauge gauge;
  int64_t now = feed(gauge, 3950, false, kWindow, 0);
  const int start = gauge.percent();
  check(start > 0 && start < 100, "a partly charged cell on battery");
  now = feed(gauge, 4000, false, kWindow, now);
  check(gauge.percent() == start, "a lighter load for a window does not raise it");
  now = feed(gauge, 3950, false, kWindow, now);
  now = feed(gauge, 3700, false, kWindow / 2, now);
  check(gauge.percent() == start, "a load for less than half the window is filtered out");
  now = feed(gauge, 3950, false, kWindow, now);
  feed(gauge, 3800, false, kWindow / 2 + 1, now);
  check(gauge.percent() < start, "a lasting lower voltage lowers it");
}

void usbChanges() {
  BatteryGauge gauge;
  int64_t now = feed(gauge, 3800, false, kWindow, 0);
  const int onBattery = gauge.percent();
  now = feed(gauge, 3950, true, kWindow - 1, now);
  check(gauge.percent() == onBattery, "plugging in holds the value until the window is full");
  now = feed(gauge, 3950, true, 1, now);
  check(gauge.percent() > onBattery, "then it follows the charge");

  now = feed(gauge, 4100, true, kWindow, now);
  check(gauge.percent() == 100, "charged");
  now = feed(gauge, 3900, false, kWindow - 1, now);
  check(gauge.percent() == 100, "unplugging holds the value until the window is full");
  now = feed(gauge, 3900, false, 1, now);
  const int discharged = gauge.percent();
  check(discharged < 100, "then it follows the discharge");
  feed(gauge, 3900, true, kWindow, now);
  check(gauge.percent() >= discharged, "plugging in never lowers it");
}

void startHolds() {
  BatteryGauge gauge;
  int64_t now = feed(gauge, 4050, false, 1, 0);
  const int first = gauge.percent();
  now = feed(gauge, 3700, false, kWindow / 2, now);
  check(gauge.percent() == first, "the first sample holds while the boot load fills the window");
  feed(gauge, 4050, false, kWindow / 2 + 1, now);
  check(gauge.percent() == first, "and the boot load does not count once the window is full");
}

void provisionalStart() {
  // The first sample came while Wi-Fi connected at boot; the cell was fuller than it said.
  BatteryGauge gauge;
  int64_t now = feed(gauge, 3700, false, 1, 0);
  const int first = gauge.percent();
  now = feed(gauge, 4000, false, kWindow - 1, now);
  check(gauge.percent() == BatteryGauge::estimate(4000, false) && gauge.percent() > first,
        "the first full window replaces a provisional first sample, also upwards");
  feed(gauge, 4000, false, 3 * kWindow, now);
  check(gauge.percent() == BatteryGauge::estimate(4000, false), "and holds after that");
}

void slowRise() {
  BatteryGauge gauge;
  int64_t now = feed(gauge, 3950, false, kWindow, 0);
  const int start = gauge.percent();
  now = feed(gauge, 3960, false, 4 * kWindow, now);
  check(gauge.percent() == start, "a reading less than the margin above does not raise it");
  now = feed(gauge, 4050, false, kWindow + kWindow / 2, now);
  check(gauge.percent() == start + 1, "a whole window clearly above raises it by one");
  now = feed(gauge, 4050, false, 2 * kWindow, now);
  check(gauge.percent() == start + 3, "then by one per window");
  feed(gauge, 3900, false, kWindow, now);
  check(gauge.percent() == BatteryGauge::estimate(3900, false), "a lower reading still lowers it at once");
}

// A cell that sags 0.5 mV per per mille of display load, against kReferenceLoad.
int sagged(int restingAtReference, int load) {
  return restingAtReference - (load - BatteryGauge::kReferenceLoad) / 2;
}

void loadCompensation() {
  BatteryGauge learning, blind;
  int64_t now = 0;
  for (int i = 0; i < 3 * kWindow; ++i, now += kStep) {
    const int load = i % 2 ? 450 : 50;  // a bright app and the clock take turns
    learning.sample(sagged(3950, load), false, now, load);
    blind.sample(sagged(3950, load), false, now);
  }
  check(learning.sag() > 0.45 && learning.sag() < 0.55, "the sag is learned from load steps");
  check(learning.percent() == BatteryGauge::estimate(3950, false),
        "with the sag learned the bright app does not count against the charge");
  check(blind.percent() < learning.percent(), "without the load the bright app lowers it");
  check(learning.lastCompensated() == 3950, "readings move to the reference load");

  BatteryGauge charging;
  now = 0;
  for (int i = 0; i < 2 * kWindow; ++i, now += kStep) {
    const int load = i % 2 ? 450 : 50;
    charging.sample(sagged(3950, load), true, now, load);
  }
  check(charging.sag() == 0 && charging.lastCompensated() == sagged(3950, 450),
        "on USB the charger holds the voltage: nothing is learned or moved");

  BatteryGauge steady;
  now = feed(steady, 3950, false, kWindow, 0);
  for (int i = 0; i < kWindow; ++i, now += kStep) steady.sample(3950 + (i % 3) - 1, false, now, 60 + (i % 2) * 10);
  check(steady.sag() == 0, "small load changes teach nothing");
  for (int i = 0; i < kWindow; ++i, now += kStep) steady.sample(3950 + (i % 2) * 40, false, now, i % 2 ? 450 : 50);
  check(steady.sag() == 0, "a voltage that rises with the load is not sag");
}

void steadyLoad() {
  LoadTracker tracker;
  check(tracker.steady(0) == -1 && tracker.last(0) == -1, "no load before a report");
  tracker.report(120, 0);
  check(tracker.last(2000) == 120 && tracker.steady(2000) == -1, "a new load is not steady yet");
  tracker.report(130, 4000);
  check(tracker.steady(LoadTracker::kSteadyMs) == 130, "small changes keep it steady once it held for 8 s");
  tracker.report(400, 10000);
  check(tracker.steady(16000) == -1 && tracker.steady(18000) == 400, "a step starts the wait again");
  check(tracker.last(18000 + LoadTracker::kStaleMs + 1) == -1, "without fresh reports the load is unknown");
}

// The MCU's reading is about 4 s old and the load flips every 20 s: right after a flip the reading
// still belongs to the old load.
void lateReadings() {
  const auto loadAt = [](int64_t t) { return (t / 20000) % 2 ? 350 : 50; };
  BatteryGauge waiting, eager;
  LoadTracker tracker;
  for (int64_t t = 0; t <= 40 * 60000; t += 2000) {
    tracker.report(loadAt(t), t);
    if (t % 10000 || t < 10000) continue;
    const int reading = sagged(3950, loadAt(t - 4000));
    waiting.sample(reading, false, t, tracker.steady(t));
    eager.sample(reading, false, t, tracker.last(t));
  }
  check(waiting.sag() > 0.45 && waiting.sag() < 0.55, "a steady load gives the real sag");
  check(eager.sag() < 0.25, "pairing every reading with the newest load learns far too little");
  check(waiting.percent() == BatteryGauge::estimate(3950, false), "and the charge holds through the flips");
}


// The bench run on a TC002 (2026-11-04): white at brightness 150 (591 per mille) and the apps
// (about 170) took turns every 20 s; the reading sat 30 mV lower in white, with about 8 mV of
// noise per reading. jitter: how far the apps' load moves between two 2 s reports.
BatteryGauge benchRun(int jitter) {
  BatteryGauge gauge;
  LoadTracker tracker;
  uint32_t seed = 12345;
  const auto noise = [&seed](int span) {
    seed = seed * 1103515245u + 12345u;
    return static_cast<int>((seed >> 16) % static_cast<uint32_t>(2 * span + 1)) - span;
  };
  const auto loadAt = [&](int64_t t) { return (t / 20000) % 2 ? 170 + noise(jitter) : 591; };
  int load = 591;
  for (int64_t t = 0; t <= 20 * 60000; t += 1000) {
    if (t % 2000 == 0) {
      load = loadAt(t);
      tracker.report(load, t);
    }
    if (t % 10000 || t < 10000) continue;
    const int truth = 4080 - static_cast<int>(0.07 * (((t - 3000) / 20000) % 2 ? 170 : 591)) - static_cast<int>(t / 20000);
    gauge.sample(truth + noise(8), false, t, tracker.steady(t));
  }
  return gauge;
}

void benchTimeline() {
  for (int jitter : {0, 25}) {
    const BatteryGauge gauge = benchRun(jitter);
    check(gauge.sag() > 0.056 && gauge.sag() < 0.084, "the bench run learns its 30 mV over 421 per mille");
    std::array<BatteryGauge::SagPair, 9> pairs{};
    const std::size_t count = gauge.sagPairs(pairs);
    bool alternating = count == pairs.size();
    for (std::size_t i = 0; i < count; ++i) {
      alternating = alternating && std::abs(pairs[i].toLoad - pairs[i].fromLoad) > 300 &&
                    (i == 0 || pairs[i].atMs > pairs[i - 1].atMs);
    }
    if (jitter == 0) check(alternating, "the status pairs are the white and apps readings, oldest first");
    check(gauge.sagRejected() == 0, "no pair of the bench run is too steep to be sag");
  }
}

void spacingAndUnknown() {
  BatteryGauge gauge;
  gauge.sample(-1, false, 0);
  check(gauge.percent() == -1, "an unknown voltage is no sample");
  int64_t now = feed(gauge, 3950, false, kWindow, 0);
  const int start = gauge.percent();
  for (int i = 0; i < 2 * kWindow; ++i) gauge.sample(3600, false, now + i * 30);
  check(gauge.percent() == start, "a burst of stream reports counts once");
}

}

int main() {
  estimates();
  reportedFullCharge();
  charging();
  discharging();
  usbChanges();
  startHolds();
  provisionalStart();
  slowRise();
  loadCompensation();
  steadyLoad();
  lateReadings();
  spacingAndUnknown();
  benchTimeline();
  return tc002d_test::finish("tc002d battery gauge");
}
