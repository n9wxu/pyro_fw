from kf_launch import *
print("consecutive bad readings (12 kPa low) needed to declare a launch, sigma 3")
for mode in ('raw','clip','median'):
    for q in (1e5, 1e7):
        hit = next((n for n in range(1, 40) if any(glitch(q, 3.0, mode, 12000, n, sd) for sd in range(5))), None)
        print(f"  {mode:6s} q={q:.0e}: {hit}  ({hit*20 if hit else '-'} ms of bad data)")
print("pad speed noise, quiet pad (no gusts), 10 min: rms pdot in m/s")
for q in (1e3,1e5,1e7):
    for s in (1.2,3.0,5.0):
        rng = random.Random(3); d = Det(q, s, 'clip'); acc=0; n=0
        for z in pad_noise(rng, s, 0.0, 30000):
            d.step(z)
            if d.n>500: acc+=d.x[1]**2; n+=1
        print(f"  q={q:.0e} sigma={s}: {math.sqrt(acc/n)/RHO_G:.2f} m/s")
print("clip, true noise twice the assumed sigma (assumed 3, actual 6): launch delay ms mean/max")
for q in (1e5,1e7):
    for g in (1.5,10,100):
        out=[]
        for sd in range(100):
            rng = random.Random(sd); d = Det(q, 3.0, 'clip'); acc = g*9.80665
            for i in range(900):
                t=(i-500)*DT; h=0.5*acc*t*t if t>0 else 0.0
                d.step(101325.0*math.exp(-h/8434.0)+rng.gauss(0,6.0))
                if d.launched: out.append((t-math.sqrt(2*30.48/acc))*1000); break
        print(f"  q={q:.0e} {g}g: {len(out)}/100 detected, {sum(out)/max(1,len(out)):.0f}/{max(out):.0f}")
