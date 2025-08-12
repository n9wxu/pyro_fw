#ifndef _PYRO_LIB
#define _PYRO_LIB

#include <stdbool.h>
#include <stdint.h>

#define MK1 1   // original pyro BMP280, 5 pin header
#define MK1b 2  // redesign with BMP280, 6 pin header & charger
#define MK1b2 3 // replace BMP280 with MS5607

// Data structures

typedef struct {
  float altitude;
  bool main_present;
  bool drog_present;
  bool main_fired;
  bool drog_fired;
} pyro_report;

typedef struct {
  double launch_height_trigger;  ///< altitude to climb before arming system
  double maximum_time_to_main;   ///< maximum flight time before firing the main
                                 ///< chute
  double maximum_time_to_drogue; ///< Time after apogee to wait before firing
                                 ///< the drogue chute
  double main_deploy_height; ///< Must fall to this height AGL before firing the
                             ///< main chute
  double drogue_deploy_fall; ///< Must fall this many feet from apogee before
                             ///< firing the drogue
} pyro_config;

#define ALTITUDE_COUNTS 4
// All data required by the pyro to exist between runs of the pyro algorithm
// the pyro algorithm must not have any local state.
typedef struct {
  double altitude[ALTITUDE_COUNTS]; // last 4 altitude readings
  bool waiting_to_launch;

  double altitude_sum;
  double altitude_average;

  pyro_config *config;
} pyro_context;

// interface functions
void pyro_log(char *s);

// library entry
void pyro_init_context(pyro_context *context, pyro_config *config);
void pyro_loop(pyro_context *context, double seconds, double altitude);

#endif // _PYRO_LIB