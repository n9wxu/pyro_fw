#include <pyro_lib.h>

#include "pyro_testConfig.h"

#include <stdio.h>
#include <stdlib.h>

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

void load_config_from_file(pyro_config *config) {
  if (config) {
    // load some defaults for now.  Read from a file later.
    config->launch_height_trigger = 50.0;
    config->maximum_time_to_main = 20.0;
    config->maximum_time_to_drogue = 5.0;
    config->main_deploy_height = 500.0;
    config->drogue_deploy_fall = 20.0;
  }
}

void pyro_log(char *s) { puts(s); }

int main(int argc, char **argv) {

  pyro_report theReport;
  pyro_context theContext;

  if (argc != 2) {
    printf("Error, no file argument\n");
    exit(1);
  }

  load_launch_data(argv[1]);
  double flight_time = theData[launch_data_size - 1].met;

  printf("Test v%d.%d\n", VERSION_MAJOR, VERSION_MINOR);

  double mission_elapsed_time = 0;

  while (mission_elapsed_time < flight_time) {
    pyro_loop(&theContext, mission_elapsed_time,
              get_altitude_for_time(mission_elapsed_time));
    mission_elapsed_time += 0.020; // 20 ms increments
  }

  free(theData);
  return 1;
}