#pragma once

#include "platform/tc002/daemon/ControlRegistry.h"
#include "platform/tc002/daemon/McuService.h"

// Control commands for the LED driver registers (McuService), for bench work:
//   mcu-registers                   the factory table, the values known in this run, the override
//   mcu-register-read  REG | all    queues a read
//   mcu-register-write REG R G B    queues a write of red, green and blue: the MCU's saved override
//   mcu-register-restore            back to the factory values (with MCU build 14: every register,
//                                   and the stock override cleared)
//   mcu-register-clear-override     MCU build 14: clears the stock firmware's saved override
// Registers and values are hexadecimal bytes. A queued request replies {"ok":true}; its result
// shows in mcu-registers once the MCU answered.
namespace awtrix {
namespace tc002d {

void addDriverRegisterCommands(ControlRegistry& control, McuService& mcu);

}
}
