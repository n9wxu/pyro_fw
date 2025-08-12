#include "pyro_lib.h"

void pyro_init_context(pyro_context *context, pyro_config *config) {
  for (int x = 0; x < ALTITUDE_COUNTS; x++) {
    context->altitude[x] = 0.0;
  }
  context->waiting_to_launch = true;
  context->altitude_average = 0.0;
  context->altitude_sum = 0.0;

  context->config = config;
}

void pyro_loop(pyro_context *context, double time, double altitude) {
  // update altitude
  for (int x = 0; x < ALTITUDE_COUNTS - 1; x++) {
    context->altitude[x] = context->altitude[x + 1];
  }
  context->altitude[ALTITUDE_COUNTS - 1] = altitude;

  // crude rolling average filter
  context->altitude_sum += altitude;
  context->altitude_sum -= context->altitude_average;
  context->altitude_average = context->altitude_sum / 16;

  if (context->waiting_to_launch) {
    if (context->altitude_average > context->config->launch_height_trigger) {
      context->waiting_to_launch = false;
      pyro_log("Launched");
    }
  } else { // flying
    if (altitude > context->altitude_average) {
      pyro_log("rising");
    } else if (altitude < context->altitude_average) {
      pyro_log("falling");
    } else {
      pyro_log("sitting");
    }
  }
}
