Flight use cases

**Status, 2026-09-28 (2.1.702).** These are still the intended flights. Where the firmware and the web pages differ from the stories:
- The rocket name takes up to 8 characters, so SKYSTREAK (9) does not fit.
- The Config tab has no Beep Mode; the Beep Codes tab holds the personalities.
- One Save on the Config tab saves the release choices with the rest. A release takes effect at the next reboot.
- The Flight Data tab has an Erase flight log button.
- Units still offer cm, with meters and feet.
- Launch is declared at 100 ft and 5 m/s, held 100 ms. The log's T+0 is the first sample 50 cm above the pad (FLT-LAUNCH-03, FLT-LAUNCH-07).
- A 0 s delay fires at apogee detection, about 0.4 s after the peak.
- The emergency main fires once the drogue has had 2 s and the rocket has then fallen faster than 35 m/s for 1 s without being slowed (FLT-EMRG-01, DD-028): about 3 s, as in the third story.

# Happy Path
I attach a new pyro mk1c to a usb port on a pc.  I point a browser at pyro.local and see the Pyro MK1C web pages.  The status shows pad_idle and the altitude is 0.0m.
Checking the Config, I give my rocket the name SKYSTREAK and the id SKY001.  I set the units to feet.  I don't know why there is a choice of cm.  That makes no sense.  Beep Mode of Digits is OK.  Pyro 1 (Drogue) fits my rocket.  I set to 0 seconds after apogee.  Pyro 2 (Main) is set for 500 ft AGL.  I do not release the pyro channels to LUA.  I press save.  I am confused about the "Save Release" option.  Maybe that is for the release choices.  I went to the Flight Data tab and I see what appears to be stale data.  I would like to clear this but I don't see a button for that.  Visiting the beep codes tab I will leave the Personality to default and the repeat to twice.  I press save beep codes and move to Lua.  I make no change to lua and move to update and leave it alone.
I attach a battery and let it charge and then assemble the unit into a electronics bay.
In the field I assemble the rocket on the bench, connecting the pyro charges for the drogue to channel 1 and the main chute charges to channel 2.  I power up the pyrocontroller on the bench and hear a chirp indicating OK to fly.  The chirp repeats after 5 seconds.  I power down the unit attach the mechanical disconnect and close up the rocket.
I carry the rocket to the launch pad, put the rocket on the rails and hook up the launch control wires to the motor.  Then I power the mk1c and remove the mechanical disconnect.  The mk1c gives 1 chirp.  5 seconds later it gives a second chirp.  I leave the area.
The rocket sits on the pad for 10 minutes until it is time to launch.  The launch controller fires the motor and rocket takes off.  The rocket transitions to flight mode as soon as the rocket is rising for more than 1 second and clears 50ft.  After 1.2 seconds the motor burns out and the rocket coasts for 15 seconds where it reaches an appogee of about 800ft.  It arcs over the apogee, the vertical speed drops to 0 then goes negative.  Pyro 1 (drogue) fires deploying a drogue chute and splitting the rocket for a slower descent.  The rocket falls for 300ft and as soon as it crosses 500ft Pyro 2 (main) fires deploying the main chute and the rocket descends to the earth.  As I walk up to retrieve the rocket I hear it beeping 8 - 2 - 4 (824ft) with a small pause.  When I reach the rocket I turn off the pyro and carry the rocket back.  Another successful flight.


# Pad Fail
I prepare the rocket with the mk1c, a drogue and a main chute.  I place the rocket on the pad disconnect the mechanical disconnect and power the mk1c.  It beeps 5 times.  It pauses 5 seconds and beeps 5 times again.  I disconnect power and install the disconnect.  Then carefully inspect the drogue pyro charge.  I see that the e-fuse has disconnected.  I reconnect the e-fuse, close the rocket, remove the disconnect and power the mk1c.  It gives a chirp and 5 seconds later a second chirp.  I leave and have a successful flight.


# Pad flight fail
I prepare the rocket with mk1c, droge and main chutes.  I launch the rocket and it clears 5000 ft.  I hear a pop, but the drogue does not deploy.  The rocket begins a ballistic descent picking up speed.  After what feels like an eternity but is probably only 3 seconds I see the main chute deploy and hear the pop of the second charge.  The emergency deployment detected an overspeed with continued acceleration on descent and fired the main chute.  It is a long walk to get the rocket as it drifts down wind but at least it did not crash.