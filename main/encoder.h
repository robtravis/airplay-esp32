/**
 * @file encoder.h
 * @brief Rotary encoder → volume.
 */
#pragma once

#include "esp_err.h"

/// Start quadrature decoding and the volume task. No-op when
/// CONFIG_ENCODER_A_GPIO is -1.
esp_err_t encoder_init(void);
