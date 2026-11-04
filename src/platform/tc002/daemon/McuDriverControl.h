#pragma once

#include "platform/tc002/daemon/ControlRegistry.h"
#include "platform/tc002/daemon/McuService.h"

// Control commands for the LED driver registers (McuService), for bench work:
//   mcu-registers                   the registers read so far, their first and current values
//   mcu-register-read  REG | all    queues a read
//   mcu-register-write REG R G B    queues a write of red, green and blue, then a read back
//   mcu-register-restore            writes back every changed register's first value
// Registers and values are hexadecimal bytes. A queued request replies {"ok":true}; its result
// shows in mcu-registers once the MCU answered.
namespace awtrix {
namespace tc002d {

void addDriverRegisterCommands(ControlRegistry& control, McuService& mcu);

}
}
