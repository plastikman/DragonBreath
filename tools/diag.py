#!/usr/bin/env python3
# DragonBreath heater diagnostic logger.
#
# Run this on any machine on the same network as the DragonBreath while a print /
# heat cycle is running. It samples the device at 2 Hz over the read-only API and
# logs chamber + PTC-element temperature, the SSR output state, the mode, and any
# fault (with its reason), while tracking the running peak element temperature. It
# both prints live and writes a CSV you can send back for analysis.
#
# It is READ-ONLY — it never commands the heater, so it is safe to run at any time.
#
# Usage:
#   python3 tools/diag.py                       # uses http://dragonbreath.local
#   python3 tools/diag.py 10.0.0.42             # explicit IP/hostname
#   python3 tools/diag.py dragonbreath.local mytoken   # if a control token is set
#
# Stop with Ctrl-C. Output CSV: dragonbreath_diag.csv in the current directory.
#
# What the columns mean:
#   ptc_c        PTC heater-element temperature (the one the 105 C cutoff watches)
#   chamber_c    chamber air temperature (the set-point sensor)
#   out          1 = SSR energized this sample, 0 = off
#   mode         off / power_on / auto / drying  (a fault forces mode=off)
#   fault        1 = a safety fault is latched (see reason), 0 = healthy
#   peak_c       running maximum ptc_c since the log started
import sys, json, time, urllib.request

HOST  = sys.argv[1] if len(sys.argv) > 1 else "dragonbreath.local"
TOKEN = sys.argv[2] if len(sys.argv) > 2 else "web"   # control-token value, or "web" if none set
BASE  = HOST if HOST.startswith("http") else "http://" + HOST
CSV   = "dragonbreath_diag.csv"

def get(path):
    req = urllib.request.Request(BASE + path)
    req.add_header("X-DragonBreath-Auth", TOKEN)
    with urllib.request.urlopen(req, timeout=4) as r:
        return json.load(r)

def main():
    try:
        info = get("/api/v2/info")
        print(f"# device={info.get('device_id')} fw={info.get('firmware')} rref_kohm={info.get('rref_kohm')}")
    except Exception as e:
        print(f"# could not reach {BASE}/api/v2/info : {e}")
        print("# check the host/IP (and token if you set one) and that you're on the same network.")
        return
    t0 = time.time(); peak = 0.0; last_fault = False
    with open(CSV, "w") as f:
        f.write("t_s,chamber_c,ptc_c,ptc_status,out,mode,fault,reason,peak_c\n")
    print(" t(s) chamber  ptc(elem) out    mode      peak   note", flush=True)
    while True:
        try:
            s = get("/api/v2/state")
        except Exception as e:
            print(f"  err {e}", flush=True); time.sleep(1); continue
        ch = s["sensors"]["chamber"]; pt = s["sensors"]["ptc"]
        o = s["heater"]["output"]; m = s["mode"]; saf = s.get("safety", {})
        faulted = bool(saf.get("fault_latched"))
        if pt["status"] == "ok" and pt["temperature_c"] > peak:
            peak = pt["temperature_c"]
        note = ""
        if pt["status"] == "ok" and pt["temperature_c"] >= 100: note += "BAND "
        if faulted and not last_fault: note += f"*** FAULT {saf.get('reason')} ***"
        last_fault = faulted
        t = time.time() - t0
        print(f" {t:4.0f} {ch['temperature_c']:6.1f} {pt['temperature_c']:8.1f}  {str(o):5} {m:8} {peak:5.1f}  {note}", flush=True)
        with open(CSV, "a") as f:
            f.write(f"{t:.1f},{ch['temperature_c']:.1f},{pt['temperature_c']:.1f},{pt['status']},"
                    f"{int(bool(o))},{m},{int(faulted)},{saf.get('reason') or ''},{peak:.1f}\n")
        time.sleep(0.5)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n# stopped. CSV written to", CSV)
