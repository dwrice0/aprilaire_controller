#pragma once
#include <stdbool.h>
#include <stddef.h>

bool provisioning_is_needed(void);
void provisioning_start(void);
bool provisioning_get_credentials(char *ssid, size_t ssid_len,
                                  char *password, size_t pass_len);
/* provisioning.h */
bool provisioning_get_mqtt_config(char *host, size_t host_len,
                                  int  *port,
                                  char *user, size_t user_len,
                                  char *pass, size_t pass_len);
void provisioning_clear(void);