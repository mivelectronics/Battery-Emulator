#ifndef __HW_MIV_S32K_H__
#define __HW_MIV_S32K_H__

/*
MIV variant A+: the Battery-Emulator core running on an NXP S32K312 under Zephyr (s32k/app/emulator).
There are no ESP32 pins here. CAN goes through the S32K FlexCAN channels (s32k/app/emulator/port). The
numbers below are indices of the pin list in the board overlay of s32k/app/emulator/boards: be-gpios in
frdm_a_s32k312.overlay, be-lines (lines named in the board devicetree) in miv_be_main.overlay. Everything
else stays at the base class default GPIO_NUM_NC.
*/

class MivS32kHal : public Esp32Hal {
 public:
  const char* name() { return "MIV S32K312"; }

  virtual uint8_t SD_SPI_BUS() { return 0; }

  virtual gpio_num_t POSITIVE_CONTACTOR_PIN() { return GPIO_NUM_0; }
  virtual gpio_num_t NEGATIVE_CONTACTOR_PIN() { return GPIO_NUM_1; }
  virtual gpio_num_t PRECHARGE_PIN() { return GPIO_NUM_2; }
  virtual gpio_num_t SECOND_BATTERY_CONTACTORS_PIN() { return GPIO_NUM_3; }
  virtual gpio_num_t BMS_POWER() { return GPIO_NUM_4; }
  virtual gpio_num_t EQUIPMENT_STOP_PIN() { return GPIO_NUM_5; }

  std::vector<comm_interface> available_interfaces() {
    return {comm_interface::CanNative, comm_interface::CanFdAddonMcp2518, comm_interface::CanFdAddonMcp2518_2};
  }
};

#endif
