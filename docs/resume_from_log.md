# Resuming a flight: fresh data, and what the log could restore

Any restart checks whether a flight is in progress and resumes it
(FLT-BROWN-02, DD-086). This note sets out what a resumed flight loses, and
the option DD-086 holds open: restoring state by replaying the flight log.
The option is not built.

## Baseline (ruled)

A resumed flight assumes no channel has fired and fires as soon as fresh
sensor data dictates. A fire into a spent igniter is harmless, so nothing
about the channels need be stored. What the baseline loses:

| Lost | Effect | Early or late |
|---|---|---|
| Time of apogee | DELAY counts in full from the resume | late |
| Peak pressure | FALLEN measures from the resume point | late |
| Whether apogee was declared | resumed in descent it is taken as passed; resumed climbing, the estimators start again and apogee waits for them to explain the readings (FLT-APO-07) | late |
| T+0, peak, fire and re-fire counts | the summary and the altitude beep-out are of the part after the restart | — |
| The log written before the restart | the resumed flight's log, as it opens, removes it (`flight_log.bin`) | — |

Every loss errs late. None can fire early.

## Option: restore from the log

The flight log holds every sample and every event row at its own time
(FLT-LOG-07): LAUNCH, ARMED, APOGEE, each fire. Replaying the events alone
would give back T+0, the apogee time, the peak, the phase and the channels
fired, and the flight would continue as one record.

What has to be true for it to be used:

- **Fast enough.** The replay has a time budget inside the restart. If it
  is not finished, the flight resumes on the baseline. The budget and the
  read time per board are to be measured; a log late in a long flight is
  the worst case, and reading events without walking every sample row may
  need an index or a scan from the end.
- **This flight's log.** The record must be tied to the resume state, so a
  log left by an earlier flight is never replayed. Clearing all resume
  state after a flight is what guarantees it. The replay reads the log
  before the resumed flight's log opens and removes it.
- **A torn tail.** The log is synced every second (FLT-LOG-06), and a
  restart finds it as its last sync left it (DD-105), so the last second
  before the restart may be missing. An event lost there falls back to the
  baseline for that item: a fire not seen is a channel fired again,
  harmlessly.
- **The outage is not measured.** The board has no clock across a restart.
  Taking the outage as zero makes every restored timer run long, so DELAY
  can still only be late.
- **Fresh data still rules.** Restored state is where the flight was. The
  airborne test and every trigger are judged on samples taken after the
  restart.

## To measure

- Time from restart to first decision, baseline and with replay, on each
  board's storage, at 1, 10 and 30 minutes into a flight.
- DELAY and FALLEN lateness on the baseline against the restored flight,
  for a restart at each phase.
