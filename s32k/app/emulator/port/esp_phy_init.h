/* comm_nvm.cpp asks for this; the S32K has no WiFi PHY to calibrate. */
#ifndef ESP_PHY_INIT_H
#define ESP_PHY_INIT_H

typedef int esp_err_t;
#ifndef ESP_OK
#define ESP_OK 0
#endif
inline esp_err_t esp_phy_erase_cal_data_in_nvs() {
  return ESP_OK;
}

#endif
