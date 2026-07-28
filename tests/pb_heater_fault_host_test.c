// SPDX-License-Identifier: MIT
// Host test for the SAFETY-CRITICAL boot-time fault-restore decision
// (pb_heater_fault_decide) — a pure header inline, verified without an NVS
// backend. This logic is what guarantees a device that latched a safety fault
// comes back up LOCKED after a power cycle, and fails SAFE (latched) whenever the
// persisted fault state cannot be read reliably.
#include "pb_heater.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        exit(1); \
    } \
} while (0)

// Float-equality check for the foldback duty (values are exact-ish ratios of 5).
#define FEQ(a, b) (fabsf((a) - (b)) < 1e-4f)

int main(void)
{
    pb_fault_reason_t code;

    // Fresh device: the NVS namespace was never written -> NOT latched.
    CHECK(pb_heater_fault_decide(false, /*ns_not_found=*/true,
                                 false, false, 0, 0, &code) == false);
    CHECK(code == PB_FAULT_NONE);

    // Namespace exists but the latch key was never written -> NOT latched.
    CHECK(pb_heater_fault_decide(true, false,
                                 false, /*latch_not_found=*/true, 0, 0, &code) == false);
    CHECK(code == PB_FAULT_NONE);

    // Latched with a valid stored code -> come up latched with that exact code.
    CHECK(pb_heater_fault_decide(true, false, /*latch_read_ok=*/true, false,
                                 1, PB_FAULT_CHAMBER_OVERTEMP, &code) == true);
    CHECK(code == PB_FAULT_CHAMBER_OVERTEMP);

    // Latch flag == 0 -> not latched even if a stale code sits in NVS.
    CHECK(pb_heater_fault_decide(true, false, true, false,
                                 0, PB_FAULT_PTC_OVERTEMP, &code) == false);
    CHECK(code == PB_FAULT_NONE);

    // Latched but the stored code is out of range (corrupt) -> latch anyway,
    // mapped to a generic cause rather than trusting the garbage byte.
    CHECK(pb_heater_fault_decide(true, false, true, false, 1, 250, &code) == true);
    CHECK(code == PB_FAULT_EMERGENCY);

    // Latched with a NONE(0) code is inconsistent -> also generic, still latched.
    CHECK(pb_heater_fault_decide(true, false, true, false,
                                 1, PB_FAULT_NONE, &code) == true);
    CHECK(code == PB_FAULT_EMERGENCY);

    // FAIL-SAFE: a genuine nvs_open error (not "namespace not found") -> latch.
    CHECK(pb_heater_fault_decide(/*open_ok=*/false, false, false, false,
                                 0, 0, &code) == true);
    CHECK(code == PB_FAULT_NVS_UNREADABLE);

    // FAIL-SAFE: opened OK but the latch key read genuinely failed -> latch.
    CHECK(pb_heater_fault_decide(true, false, /*latch_read_ok=*/false,
                                 /*latch_not_found=*/false, 0, 0, &code) == true);
    CHECK(code == PB_FAULT_NVS_UNREADABLE);

    puts("pb_heater fault-restore checks: PASS");

    // --- Element-foldback hysteresis (pb_heater_foldback_cut) -------------------
    // Per-Rref thresholds: 33k board gets the conservative pair, anything else the 82k
    // pair. Resolve both via the helper and exercise the hysteresis on each.
    float cut82, res82, cut33, res33;
    pb_heater_foldback_thresholds(82, &cut82, &res82);
    pb_heater_foldback_thresholds(33, &cut33, &res33);
    CHECK(cut82 == PB_HEATER_PTC_FOLDBACK_CUT_82K_C && res82 == PB_HEATER_PTC_FOLDBACK_RESUME_82K_C);
    CHECK(cut33 == PB_HEATER_PTC_FOLDBACK_CUT_33K_C && res33 == PB_HEATER_PTC_FOLDBACK_RESUME_33K_C);
    CHECK(cut33 < cut82);                                   // 33k board cuts earlier
    // Unknown/other Rref (incl. 0) falls back to the 82k pair.
    float cutx, resx; pb_heater_foldback_thresholds(0, &cutx, &resx);
    CHECK(cutx == cut82 && resx == res82);
    // Hysteresis behavior against each board's own thresholds.
    for (int b = 0; b < 2; b++) {
        float cut_c    = b ? cut33 : cut82;
        float resume_c = b ? res33 : res82;
        float band_mid = (cut_c + resume_c) * 0.5f;
        CHECK(pb_heater_foldback_cut(true, cut_c,        false, cut_c, resume_c) == true);   // at cut -> cut
        CHECK(pb_heater_foldback_cut(true, cut_c + 5.0f, false, cut_c, resume_c) == true);
        CHECK(pb_heater_foldback_cut(true, resume_c-0.1f, true, cut_c, resume_c) == false);  // below resume -> allow
        CHECK(pb_heater_foldback_cut(true, band_mid,      true,  cut_c, resume_c) == true);  // band holds prev
        CHECK(pb_heater_foldback_cut(true, band_mid,      false, cut_c, resume_c) == false);
        CHECK(pb_heater_foldback_cut(false, 0.0f,         true,  cut_c, resume_c) == true);  // bad read holds prev
        CHECK(pb_heater_foldback_cut(false, 0.0f,         false, cut_c, resume_c) == false);
        // Every board's cut/resume stays below the hard cutoff.
        CHECK(cut_c < PB_HEATER_PTC_CUTOFF_C && resume_c < cut_c);
    }
    // Effective-foldback resolver: 0/negative user override -> per-Rref default; a
    // positive override wins with resume = cut - band. Stays below the hard cutoff.
    float ec, er;
    pb_heater_effective_foldback(0.0f, 33, &ec, &er);    // auto -> 33k pair
    CHECK(ec == cut33 && er == res33);
    pb_heater_effective_foldback(-1.0f, 82, &ec, &er);   // <=0 also auto -> 82k pair
    CHECK(ec == cut82 && er == res82);
    pb_heater_effective_foldback(95.0f, 33, &ec, &er);   // override wins over per-Rref
    CHECK(ec == 95.0f && er == 95.0f - PB_HEATER_PTC_FOLDBACK_BAND_C);
    CHECK(ec < PB_HEATER_PTC_CUTOFF_C && er < ec);
    CHECK(PB_HEATER_FB_CUT_MAX_C < PB_HEATER_PTC_CUTOFF_C);   // override ceiling < hard cutoff
    puts("pb_heater element-foldback checks: PASS");
    return 0;
}
