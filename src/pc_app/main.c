#include <pyro_lib.h>

#include "unity.h"
#include <ini.h>

#include "pyro_testConfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  double met;
  double altitude;
} launch_data_t;

launch_data_t *theData = NULL;
size_t launch_data_size = 0;

// load all the data from a CSV file of time & data
// # is a line comment
// data is time (s) , altitude (ft)
void load_launch_data(char *filename) {
  FILE *fp;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;

  fp = fopen(filename, "r");
  if (fp == NULL) {
    exit(EXIT_FAILURE);
  }

  int datacount = 0;
  while ((read = getline(&line, &len, fp)) != -1) {
    if (line[0] != '#') {
      datacount++;
    }
  }

  theData = (launch_data_t *)malloc(sizeof(launch_data_t) * datacount);
  rewind(fp);

  int index = 0;
  while ((read = getline(&line, &len, fp)) != -1) {
    if (line[0] != '#') {
      float t, a;
      sscanf(line, "%f,%f", &t, &a);
      theData[index].met = t;
      theData[index].altitude = a;
      index++;
    }
  }
  launch_data_size = index;

  printf("%d data points\n", index);
  fclose(fp);
  if (line) {
    free(line);
  }
}

// interpolate to return an altitude for the requested time
double get_altitude_for_time(double met) {
  double result = 0;
  if (met <= 0)
    return 0;

  for (size_t x = 1; x < launch_data_size; x++) {
    if (met < theData[x].met) {
      double t1 = theData[x - 1].met;
      double t2 = theData[x].met;
      double a1 = theData[x - 1].altitude;
      double a2 = theData[x].altitude;
      result = a1 + (met - t1) * (a2 - a1) / (t2 - t1);
    }
  }
  return result;
}

static int handler(void *user, const char *section, const char *name,
                   const char *value) {

  pyro_config *config = (pyro_config *)user;
#define MATCH(s, n) strcmp(section, s) == 0 && strcmp(name, n) == 0
  //  bool metric = iniparser_getboolean(ini, "global:metric", false);
  // TODO add code to normalize all internal units.

  if (MATCH("global", "LaunchHeightTrigger")) {
    config->launch_height_trigger = atof(value);
  } else if (MATCH("main", "MaxTimeToRelease")) {
    config->maximum_time_to_main = atof(value);
  } else if (MATCH("drogue", "MaxTimeToRelease")) {
    config->maximum_time_to_drogue = atof(value);
  } else if (MATCH("main", "ReleaseAltitude")) {
    config->main_deploy_height = atof(value);
  } else if (MATCH("drogue", "MaxDistanceToFall")) {
    config->drogue_deploy_fall = atof(value);
  } else {
    return 0;
  }
  return 1;
}

void load_config_from_file(char *filename, pyro_config *config) {

  if (config) {
    int error = ini_parse(filename, handler, config);

    if (error < 0) {
      fprintf(stderr, "cannot parse: %s : %d\n", filename, error);
      exit(1);
    }
  }
}

void pyro_log(char *s) { puts(s); }

bool main_fired = false;
bool drogue_fired = false;

void pyro_fire_main(void) {
  if (!main_fired) {
    puts("Fire MAIN");
    main_fired = true;
  }
}
void pyro_fire_drogue(void) {
  if (!drogue_fired) {
    puts("Fire DROGUE");
    drogue_fired = true;
  }
}

int main(int argc, char **argv) {

  pyro_report theReport;
  pyro_context theContext;
  pyro_config theConfig;

  if (argc != 3) {
    printf("Error, no file argument\n");
    exit(1);
  }

  load_launch_data(argv[1]);
  load_config_from_file(argv[2], &theConfig);

  double flight_time = theData[launch_data_size - 1].met;

  printf("Test v%d.%d\n", VERSION_MAJOR, VERSION_MINOR);

  pyro_init_context(&theContext, &theConfig);
  theContext.log = pyro_log;
  theContext.fire_main = pyro_fire_main;
  theContext.fire_drogue = pyro_fire_drogue;

  double mission_elapsed_time = 0;

  while (mission_elapsed_time < flight_time) {
    pyro_loop(&theContext, mission_elapsed_time,
              get_altitude_for_time(mission_elapsed_time));
    mission_elapsed_time += 0.020; // 20 ms increments
  }

  free(theData);
  return 1;
}