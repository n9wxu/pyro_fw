#include "pyro_lib.h"
#include <stdbool.h>

bool getReport(pyro_report *report) {
  report->altitude = 3500;
  return true;
}
