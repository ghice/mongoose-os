#include <stdbool.h>
#include <stdio.h>

#include "common/platform.h"
#include "fw/src/mg_app.h"
#include "fw/src/mg_console.h"
#include "fw/src/mg_gpio.h"
#include "fw/src/mg_hal.h"
#include "fw/src/mg_i2c.h"
#include "fw/src/mg_mongoose.h"
#include "fw/src/mg_sys_config.h"
#include "fw/src/mg_timers.h"

#define LED_GPIO 10
#define RELAY_GPIO 13
#define I2C_SDA_GPIO 12
#define I2C_SCL_GPIO 14
#define MCP9808_ADDR 0x1F

struct esp_i2c_connection {
  uint8_t sda_gpio;
  uint8_t scl_gpio;
  uint8_t started;
};

static struct esp_i2c_connection s_i2c;
static bool s_heater = false;

static double mc6808_read_temp(i2c_connection i2c) {
  double ret = -1000;
  if (i2c_start(i2c, MCP9808_ADDR, I2C_WRITE) != I2C_ACK) {
    return -1000;
  }
  i2c_send_byte(i2c, 0x05);
  if (i2c_start(i2c, MCP9808_ADDR, I2C_READ) != I2C_ACK) {
    return -1000;
  }
  uint8_t upper_byte = i2c_read_byte(i2c, I2C_ACK);
  uint8_t lower_byte = i2c_read_byte(i2c, I2C_NAK);
  i2c_stop(i2c);
  upper_byte &= 0x1f;
  if (upper_byte & 0x10) {
    upper_byte &= 0xf;
    ret = -(256 - (upper_byte * 16.0 + lower_byte / 16.0));
  } else {
    ret = (upper_byte * 16.0 + lower_byte / 16.0);
  }
  return ret;
}

static void set_heater(bool on) {
  CONSOLE_LOG(LL_INFO, ("Heater %s", (on ? "on" : "off")));
  mg_gpio_write(LED_GPIO, (on ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW));
  mg_gpio_write(RELAY_GPIO, (on ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW));
  s_heater = on;
}

static void handle_heater(struct mg_connection *nc, int ev, void *ev_data) {
  if (ev != MG_EV_HTTP_REQUEST) return;
  mg_send_response_line(nc, 200,
                        "Content-Type: text/html\r\n"
                        "Connection: close\r\n");
  double temp = mc6808_read_temp(&s_i2c);
  mg_printf(nc,
            "<h1>Welcome to Cesanta Office IoT!</h1>\r\n"
            "<p>Temperature is %.2lf&deg;C.</p>\r\n"
            "<p>Heater is %s.</p>\r\n"
            "<form action=/heater/%s><input type=submit value='Turn heater "
            "%s'></form>\r\n"
            "<hr>\r\n"
            "Heater FW %s (%s)",
            temp, (s_heater ? "on" : "off"), (s_heater ? "off" : "on"),
            (s_heater ? "off" : "on"), get_ro_vars()->fw_version,
            get_ro_vars()->fw_id);
  nc->flags |= MG_F_SEND_AND_CLOSE;
  (void) ev_data;
}

static void handle_heater_action(struct mg_connection *nc, int ev,
                                 void *ev_data) {
  if (ev != MG_EV_HTTP_REQUEST) return;
  struct http_message *hm = (struct http_message *) ev_data;
  if (mg_vcmp(&hm->uri, "/heater/on") == 0) {
    set_heater(true);
  } else if (mg_vcmp(&hm->uri, "/heater/off") == 0) {
    set_heater(false);
  }
  mg_http_send_redirect(nc, 302, mg_mk_str("/heater"), mg_mk_str(NULL));
  nc->flags |= MG_F_SEND_AND_CLOSE;
}

static void handle_debug(struct mg_connection *nc, int ev, void *ev_data) {
  if (ev != MG_EV_HTTP_REQUEST) return;
  struct http_message *hm = (struct http_message *) ev_data;
  mg_send_response_line(nc, 200,
                        "Content-Type: text/plain\r\n"
                        "Connection: close\r\n");
  mg_printf(nc, "Time is %.2lf. Free RAM %u.\r\n", mg_time(),
            mg_get_free_heap_size());
  nc->flags |= MG_F_SEND_AND_CLOSE;
  (void) hm;
}

struct mg_connection *s_sensor_conn = NULL;

static void handle_sensor_conn(struct mg_connection *nc, int ev,
                               void *ev_data) {
  switch (ev) {
    case MG_EV_HTTP_REPLY: {
      nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      break;
    }
    case MG_EV_CLOSE: {
      s_sensor_conn = NULL;
      break;
    }
  }
  (void) ev_data;
}

static void sensor_timer_cb(void *arg) {
  if (s_sensor_conn != NULL) return; /* In progress. */
  double temp = mc6808_read_temp(&s_i2c);
  if (temp <= -1000) return; /* Error */
  char *eh = NULL, *post_data = NULL;
  mg_asprintf(&post_data, 0, "{\"office_temperature\": %.2lf}", temp);
  if (get_cfg()->hsw.auth != NULL) {
    mg_asprintf(&eh, 0, "Authorization: %s\r\n", get_cfg()->hsw.auth);
  }
  s_sensor_conn =
      mg_connect_http(mg_get_mgr(), handle_sensor_conn,
                      get_cfg()->hsw.sensor_data_url, eh, post_data);
  free(eh);
  free(post_data);
  (void) arg;
}

enum mg_app_init_result mg_app_init(void) {
  mg_gpio_set_mode(LED_GPIO, GPIO_MODE_OUTPUT, GPIO_PULL_FLOAT);
  mg_gpio_set_mode(RELAY_GPIO, GPIO_MODE_OUTPUT, GPIO_PULL_FLOAT);
  mg_gpio_write(LED_GPIO, GPIO_LEVEL_LOW);
  mg_gpio_write(RELAY_GPIO, GPIO_LEVEL_LOW);
  mg_register_http_endpoint(mg_get_http_listening_conn(), "/heater/",
                            handle_heater_action);
  mg_register_http_endpoint(mg_get_http_listening_conn(), "/heater",
                            handle_heater);
  mg_register_http_endpoint(mg_get_http_listening_conn(), "/debug",
                            handle_debug);
  s_i2c.sda_gpio = I2C_SDA_GPIO;
  s_i2c.scl_gpio = I2C_SCL_GPIO;
  i2c_init(&s_i2c);

  struct sys_config_hsw *hcfg = &get_cfg()->hsw;
  if (hcfg->sensor_report_interval_ms > 0 && hcfg->sensor_data_url != NULL) {
    mg_set_c_timer(hcfg->sensor_report_interval_ms, true /* repeat */,
                   sensor_timer_cb, NULL);
  }

  return MG_APP_INIT_SUCCESS;
}