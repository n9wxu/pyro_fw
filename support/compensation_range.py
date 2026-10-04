"""Both pressure sensors' compensation, integer for integer as the firmware
does it, over every code the ADC can give and past the parts' rated ranges.

    python3 support/compensation_range.py

The question it answers is in docs/pressure_collector.md: can the arithmetic
bend, wrap or overflow anywhere? Coefficients are the datasheets' examples:
docs/datasheets/MS5607-02BA03_2017-06.pdf page 8, and
docs/datasheets/BST-BMP280-DS001-26_2021-10.pdf section 3.12.
"""
def sar(x, n): return x >> n          # Python's >> on ints is arithmetic, as C's on int64
def i32(x):
    x &= 0xffffffff
    return x - (1 << 32) if x & 0x80000000 else x

PROM = dict(C1=46372, C2=43981, C3=29059, C4=27842, C5=31553, C6=28165)  # MS5607 datasheet page 8

def ms5607(d1, d2, second_order=False, c=PROM):
    dT = d2 - (c['C5'] << 8)
    temp = 2000 + sar(dT * c['C6'], 23)
    off = (c['C2'] << 17) + sar(c['C4'] * dT, 6)
    sens = (c['C1'] << 16) + sar(c['C3'] * dT, 7)
    if second_order and temp < 2000:
        t2 = sar(dT * dT, 31); off2 = 61 * (temp - 2000) ** 2 >> 4; sens2 = 2 * (temp - 2000) ** 2
        if temp < -1500:
            off2 += 15 * (temp + 1500) ** 2; sens2 += 8 * (temp + 1500) ** 2
        temp -= t2; off -= off2; sens -= sens2
    wide = sar(sar(d1 * sens, 21) - off, 15)
    return i32(wide), wide, temp, sens

def d2_at(temp_c, c=PROM):
    return (c['C5'] << 8) + int((temp_c * 100 - 2000) * (1 << 23) / c['C6'])
def d1_at(pa, d2):
    lo, hi = 0, 1 << 24
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if ms5607(mid, d2)[1] < pa: lo = mid
        else: hi = mid
    return hi

print("MS5607, datasheet example coefficients")
print("  example: D1 6465444, D2 8077636 ->", ms5607(6465444, 8077636)[:3], "(datasheet: 110002 = 1100.02 mbar, 2000)")
for t in (-40, -15, 0, 20, 45, 85):
    d2 = d2_at(t)
    p0, pmax = ms5607(0, d2)[1], ms5607((1 << 24) - 1, d2)[1]
    step = (pmax - p0) / (1 << 24)
    # monotone and straight in D1: check second differences on a coarse grid
    grid = [ms5607(d, d2)[1] for d in range(0, 1 << 24, 4096)]
    mono = all(b >= a for a, b in zip(grid, grid[1:]))
    bend = max(abs(grid[i + 1] - 2 * grid[i] + grid[i - 1]) for i in range(1, len(grid) - 1))
    wrapped = any(ms5607(d, d2)[0] != ms5607(d, d2)[1] for d in (0, (1 << 24) - 1))
    so = ms5607(d1_at(101325, d2), d2, True)[1] - 101325
    so_hi = ms5607(d1_at(1000, d2), d2, True)[1] - 1000
    print(f"  {t:4d} C: D1=0 -> {p0:8d} Pa, D1=max -> {pmax:7d} Pa, {step * 1000:.2f} mPa/count, monotone {mono}, "
          f"worst bend {bend} Pa, int32 wraps {wrapped}; second order would move 1013 hPa by {so:+d} Pa, 10 hPa by {so_hi:+d} Pa")
print("  D2 beyond the part's range, at the D1 that reads 1013 hPa at 20 C:")
d1 = d1_at(101325, d2_at(20))
for d2 in (0, 1 << 20, d2_at(-40), d2_at(85), (1 << 24) - 1):
    r = ms5607(d1, d2)
    print(f"    D2 {d2:8d}: {r[1]:9d} Pa, {r[2] / 100:8.2f} C, SENS {'negative' if r[3] < 0 else 'positive'}")
print("  where SENS changes sign: D2 =", next(d for d in range(0, 1 << 24, 256) if ms5607(1, d)[3] > 0), "=",
      f"{ms5607(1, next(d for d in range(0, 1 << 24, 256) if ms5607(1, d)[3] > 0))[2] / 100:.0f} C")
# every PROM corner: can the 64-bit sums or the int32 result overflow?
worst = 0
for c1 in (0, 65535):
    for c2 in (0, 65535):
        for c3 in (0, 65535):
            for c4 in (0, 65535):
                for c5 in (0, 65535):
                    c = dict(C1=c1, C2=c2, C3=c3, C4=c4, C5=c5, C6=65535)
                    for d1 in (0, (1 << 24) - 1):
                        for d2 in (0, (1 << 24) - 1):
                            dT = d2 - (c5 << 8)
                            sens = (c1 << 16) + (c3 * dT >> 7); off = (c2 << 17) + (c4 * dT >> 6)
                            worst = max(worst, abs(d1 * sens), abs(off), abs(dT * 65535))
print(f"  largest intermediate at any PROM and any codes: 2^{worst.bit_length()} (int64 holds 2^63)")

# BMP280, datasheet section 3.12 example trimming values
T = dict(T1=27504, T2=26435, T3=-1000)
P = dict(P1=36477, P2=-10685, P3=3024, P4=2855, P5=140, P6=-7, P7=15500, P8=-14600, P9=6000)
def bmp280(adc_p, adc_t):
    v1 = sar(((adc_t >> 3) - (T['T1'] << 1)) * T['T2'], 11)
    v2 = sar(sar(((adc_t >> 4) - T['T1']) ** 2, 12) * T['T3'], 14)
    t_fine = v1 + v2
    temp = sar(t_fine * 5 + 128, 8)
    a = t_fine - 128000
    b = a * a * P['P6'] + ((a * P['P5']) << 17) + (P['P4'] << 35)
    a = sar(a * a * P['P3'], 8) + ((a * P['P2']) << 12)
    a = sar(((1 << 47) + a) * P['P1'], 33)
    if a == 0: return None, temp
    p = 1048576 - adc_p
    q = ((p << 31) - b) * 3125
    p = int(q / a)                      # C's division truncates toward zero
    a2 = sar(P['P9'] * (p >> 13) * (p >> 13), 25)
    b2 = sar(P['P8'] * p, 19)
    p = sar(p + a2 + b2, 8) + (P['P7'] << 4)
    return p / 256.0, temp
print("\nBMP280, datasheet example trimming values")
print("  example: adc_P 415148, adc_T 519888 ->", bmp280(415148, 519888), "(datasheet: 100653.27 Pa, 2508)")
for adc_t in (300000, 420000, 519888, 620000, 700000):
    vals = [(a, bmp280(a, adc_t)[0]) for a in range(0, 1 << 20, 256)]
    temp = bmp280(0, adc_t)[1]
    mono = all(b[1] < a[1] for a, b in zip(vals, vals[1:]))
    slopes = [vals[i + 1][1] - vals[i][1] for i in range(len(vals) - 1)]
    print(f"  {temp / 100:6.1f} C: adc_P=0 -> {vals[0][1]:9.0f} Pa, adc_P=max -> {vals[-1][1]:9.0f} Pa, monotone {mono}, "
          f"Pa per count {min(slopes) / 256:.4f} to {max(slopes) / 256:.4f}")
    for target in (30000, 10000, 1000, 100):
        a = next((x for x, v in vals if v < target), None)
        print(f"      first code under {target:6d} Pa: {a}")
