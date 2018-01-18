/*
 * Copyright (c) 2014-2017 Cesanta Software Limited
 * All rights reserved
 */

#include "esp32_bt.h"
#include "esp32_bt_internal.h"

#include <stdbool.h>
#include <stdlib.h>

#include "bta_api.h"
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "nvs.h"

#include "common/mg_str.h"

#include "mgos_hal.h"
#include "mgos_net.h"
#include "mgos_sys_config.h"

const char *esp32_bt_addr_to_str(const esp_bd_addr_t addr, char *out) {
  return mgos_bt_addr_to_str((const struct mgos_bt_addr *) &addr[0], out);
}

bool esp32_bt_addr_from_str(const struct mg_str addr_str, esp_bd_addr_t addr) {
  return mgos_bt_addr_from_str(addr_str, (struct mgos_bt_addr *) &addr[0]);
}

int esp32_bt_addr_cmp(const esp_bd_addr_t a, const esp_bd_addr_t b) {
  return mgos_bt_addr_cmp((const struct mgos_bt_addr *) &a[0],
                          (const struct mgos_bt_addr *) &b[0]);
}

bool esp32_bt_addr_is_null(const esp_bd_addr_t addr) {
  return mgos_bt_addr_is_null((const struct mgos_bt_addr *) &addr[0]);
}

const char *bt_uuid128_to_str(const uint8_t *u, char *out) {
  sprintf(out,
          "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
          "%02x%02x%02x%02x%02x%02x",
          u[15], u[14], u[13], u[12], u[11], u[10], u[9], u[8], u[7], u[6],
          u[5], u[4], u[3], u[2], u[1], u[0]);
  return out;
}

const char *mgos_bt_uuid_to_str(const esp_bt_uuid_t *uuid, char *out) {
  switch (uuid->len) {
    case ESP_UUID_LEN_16: {
      sprintf(out, "%04x", uuid->uuid.uuid16);
      break;
    }
    case ESP_UUID_LEN_32: {
      sprintf(out, "%08x", uuid->uuid.uuid32);
      break;
    }
    case ESP_UUID_LEN_128: {
      bt_uuid128_to_str(uuid->uuid.uuid128, out);
      break;
    }
    default: { sprintf(out, "?(%u)", uuid->len); }
  }
  return out;
}

bool mgos_bt_uuid_from_str(const struct mg_str uuid_str, esp_bt_uuid_t *uuid) {
  bool result = false;
  struct mg_str uuid_str_nul = mg_strdup_nul(uuid_str);
  if (uuid_str.len == 36) {
    unsigned int u[16];
    if (sscanf(uuid_str_nul.p,
               "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
               "%02x%02x%02x%02x%02x%02x",
               &u[15], &u[14], &u[13], &u[12], &u[11], &u[10], &u[9], &u[8],
               &u[7], &u[6], &u[5], &u[4], &u[3], &u[2], &u[1], &u[0]) == 16) {
      result = true;
      uuid->len = ESP_UUID_LEN_128;
      for (int i = 0; i < 16; i++) {
        uuid->uuid.uuid128[i] = u[i];
      }
    }
  } else if (uuid_str.len <= 8) {
    unsigned int u;
    if (sscanf(uuid_str_nul.p, "%08x", &u) == 1) {
      result = true;
      if (u & 0xffff0000) {
        uuid->len = ESP_UUID_LEN_32;
        uuid->uuid.uuid32 = u;
      } else {
        uuid->len = ESP_UUID_LEN_16;
        uuid->uuid.uuid16 = u;
      }
    }
  }
  free((void *) uuid_str_nul.p);
  return result;
}

int mgos_bt_uuid_cmp(const esp_bt_uuid_t *a, const esp_bt_uuid_t *b) {
  int result = 0;
  if (a->len == ESP_UUID_LEN_128 || b->len == ESP_UUID_LEN_128) {
    /* 128-bit UUID is always > 16 or 32-bit */
    if (a->len != ESP_UUID_LEN_128 && b->len == ESP_UUID_LEN_128) {
      result = -1;
    } else if (a->len == ESP_UUID_LEN_128 && b->len != ESP_UUID_LEN_128) {
      result = 1;
    } else {
      for (int i = 15; i >= 0; i--) {
        uint8_t va = a->uuid.uuid128[i];
        uint8_t vb = b->uuid.uuid128[i];
        if (va != vb) {
          result = (va > vb ? 1 : -1);
          break;
        }
      }
    }
  } else {
    uint32_t va, vb;
    va = (a->len == ESP_UUID_LEN_16 ? a->uuid.uuid16 : a->uuid.uuid32);
    vb = (b->len == ESP_UUID_LEN_16 ? b->uuid.uuid16 : b->uuid.uuid32);
    if (va < vb) {
      result = -1;
    } else if (va > vb) {
      result = 1;
    }
  }
  return result;
}

enum cs_log_level ll_from_status(esp_bt_status_t status) {
  return (status == ESP_BT_STATUS_SUCCESS ? LL_DEBUG : LL_ERROR);
}

static void mgos_bt_net_ev(int ev, void *evd, void *arg) {
  if (ev != MGOS_NET_EV_IP_ACQUIRED) return;
  LOG(LL_INFO, ("Network is up, disabling Bluetooth"));
  mgos_sys_config_set_bt_enable(false);
  char *msg = NULL;
  if (save_cfg(&mgos_sys_config, &msg)) {
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
  }
  (void) arg;
}

int mgos_bt_ble_get_num_paired_devices(void) {
  return esp_ble_get_bond_device_num();
}

/* Workaround for https://github.com/espressif/esp-idf/issues/1406 */
bool esp32_bt_wipe_config(void) {
  bool result = false;
  nvs_handle h = 0;
  /* CONFIG_FILE_PATH form btc_config.c */
  if (nvs_open("bt_config.conf", NVS_READWRITE, &h) != ESP_OK) goto clean;
  if (nvs_erase_key(h, "bt_cfg_key") != ESP_OK) goto clean;
  result = true;

clean:
  if (h != 0) nvs_close(h);
  return result;
}

bool mgos_bt_common_init(void) {
  bool ret = false;
  if (!mgos_sys_config_get_bt_enable()) {
    LOG(LL_INFO, ("Bluetooth is disabled"));
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    return true;
  }

  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_bt_controller_init(&bt_cfg);
  if (err) {
    LOG(LL_ERROR, ("BT init failed: %d", err));
    goto out;
  }
  err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
  if (err) {
    LOG(LL_ERROR, ("BT enable failed: %d", err));
    goto out;
  }
  err = esp_bluedroid_init();
  if (err != ESP_OK) {
    LOG(LL_ERROR, ("bluedroid init failed: %d", err));
    goto out;
  }
  err = esp_bluedroid_enable();
  if (err != ESP_OK) {
    LOG(LL_ERROR, ("bluedroid enable failed: %d", err));
    goto out;
  }

  if (!esp32_bt_gap_init()) {
    LOG(LL_ERROR, ("GAP init failed"));
    ret = false;
    goto out;
  }

  esp_ble_gatt_set_local_mtu(ESP_GATT_MAX_MTU_SIZE);

  if (!esp32_bt_gattc_init()) {
    LOG(LL_ERROR, ("GATTC init failed"));
    ret = false;
    goto out;
  }

  if (!esp32_bt_gatts_init()) {
    LOG(LL_ERROR, ("GATTS init failed"));
    ret = false;
    goto out;
  }

  if (!mgos_sys_config_get_bt_keep_enabled()) {
    mgos_event_add_group_handler(MGOS_EVENT_GRP_NET, mgos_bt_net_ev, NULL);
  }

  LOG(LL_INFO, ("Bluetooth init ok, pairing %s, %d paired devices",
                (mgos_bt_gap_get_pairing_enable() ? "enabled" : "disabled"),
                mgos_bt_ble_get_num_paired_devices()));
  ret = true;

out:
  return ret;
}
