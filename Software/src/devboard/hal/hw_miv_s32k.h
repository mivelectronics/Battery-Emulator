#ifndef __HW_MIV_S32K_H__
#define __HW_MIV_S32K_H__

/*
MIV variant A+: the Battery-Emulator core running on an NXP S32K312 under Zephyr (s32k/app/emulator).
There are no ESP32 pins here. CAN goes through the S32K FlexCAN channels (s32k/app/emulator/port), so
every pin stays at the base class default GPIO_NUM_NC. Contactor outputs come with the MIV board.
*/

class MivS32kHal : public Esp32Hal {
 public:
  const char* name() { return "MIV S32K312"; }

  virtual uint8_t SD_SPI_BUS() { return 0; }

  std::vector<comm_interface> available_interfaces() {
    return {comm_interface::CanNative, comm_interface::CanFdAddonMcp2518, comm_interface::CanFdAddonMcp2518_2};
  }
};

#endif
