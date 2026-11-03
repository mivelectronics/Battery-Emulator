#ifndef __HW_MIV_ESP32ETH_H__
#define __HW_MIV_ESP32ETH_H__

/*
MIV / KMP Electronics ESP32ETH_8R8I_5Av5 ("SMART8 ESP32 Eth V1.5"), ESP32-WROOM-32E, 4 MB flash.

Network side of the MIV split design: the ESP32 runs the web UI, MQTT, OTA and WiFi, while an
NXP S32K312 does the CAN work. There is no CAN transceiver on the ESP32: with -D CAN_VIA_LINK the CAN
interfaces are served by the S32K over the link UART (bridge mode, can_link.h).

- Ethernet: WIZnet W5500 on VSPI (SCK 18, MISO 19, MOSI 23), CS 33, INT 5, RST 12 (ETHERNET_W5500).
  The MCP23S17 (8 relays, 8 inputs) shares VSPI with CS 32 and is held deselected for now.
- RS485: ST3485, RO -> IO4, DI <- IO16, DE+/RE on GPIO2 (strapping pin, low at boot).
- LED: WS2812 on GPIO0 (strapping pin; the LED's data line is a load only).
- Button B1 on IO15.
- Link to the S32K: UART2, TX IO17 (J11 pin 4), RX I39 (J11 pin 7), see FIRMWARE_PLAN.md.
*/

class MivEsp32EthHal : public Esp32Hal {
 public:
  const char* name() { return "MIV ESP32ETH_8R8I (W5500)"; }

  virtual void set_default_configuration_values() {}

  // RS485 (ST3485). Driver enable and receiver enable are tied together on GPIO2.
  virtual gpio_num_t RS485_TX_PIN() { return GPIO_NUM_16; }
  virtual gpio_num_t RS485_RX_PIN() { return GPIO_NUM_4; }
  virtual gpio_num_t RS485_DE_PIN() { return GPIO_NUM_2; }

  // LED
  virtual gpio_num_t LED_PIN() { return GPIO_NUM_0; }
  virtual uint8_t LED_MAX_BRIGHTNESS() { return 40; }

  // Momentary push-button that can be long-pressed at runtime to start the Wi-Fi AP.
  virtual gpio_num_t AP_BUTTON_PIN() { return GPIO_NUM_15; }

  // W5500 Ethernet and the link UART: see hw_miv_esp32eth_pins.h

  std::vector<comm_interface> available_interfaces() {
#ifdef CAN_VIA_LINK
    // CAN controllers on the S32K behind the link UART: CAN0 (inverter, on-board transceiver) shows up
    // as "native CAN", CAN1 and CAN4 (CAN FD) as the two CAN FD interfaces.
    return {
        comm_interface::Modbus,        comm_interface::RS485, comm_interface::CanNative,
        comm_interface::CanFdAddonMcp2518, comm_interface::CanFdAddonMcp2518_2,
    };
#else
    return {
        comm_interface::Modbus,
        comm_interface::RS485,
    };
#endif
  }
};

#endif  // __HW_MIV_ESP32ETH_H__
