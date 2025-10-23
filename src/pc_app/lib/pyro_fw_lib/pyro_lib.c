#include "pyro_lib.h"

void pyro_init_context(pyro_context *context, pyro_config *config) {
  context->altitude = 0.0;
  context->waiting_to_launch = true;
  context->altitude_average = 0.0;
  context->altitude_sum = 0.0;

  context->maximum_altitude = 0.0;

  context->config = config;
}

void pyro_loop(pyro_context *context, double time, double altitude) {
  context->altitude = altitude;

  // crude rolling average filter
  context->altitude_sum += altitude;
  context->altitude_sum -= context->altitude_average;
  context->altitude_average = context->altitude_sum / 16;

  if (altitude > context->maximum_altitude) {
    context->maximum_altitude = altitude;
  }

  if (context->waiting_to_launch) {
    if (context->altitude_average > context->config->launch_height_trigger) {
      context->waiting_to_launch = false;
      context->log("Launched");
    }
  } else { // flying
    if (altitude > context->altitude_average) {
      context->log("rising");
    } else if (altitude < context->altitude_average) {
      context->log("falling");
      if (context->maximum_altitude - context->altitude_average >
          context->config->drogue_deploy_fall) {
        context->fire_drogue();
      }
      if (context->altitude < context->config->main_deploy_height) {
        context->fire_main();
      }
    } else {
      context->log("sitting");
    }
  }
}
