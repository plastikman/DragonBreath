#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
httpd="$root/components/pb_httpd/pb_httpd.c"
adapter="$root/components/db_portal/db_portal.c"
core_portal="$root/managed_components/dc_portal/dc_portal.c"
diagnostics="$root/components/db_portal/www/diagnostics.html"
consequential_toggle="$root/components/db_portal/www/consequential-toggle.js"
# The dashboard/control UI is supplied by the pinned dragon-core dc_ui component.
portal="${DC_UI_HTML:-}"
if [ -z "$portal" ]; then
    for candidate in \
        "$root/managed_components/dc_ui/www/app.html" \
        "$root/../dragon-core/components/dc_ui/www/app.html"
    do
        if [ -f "$candidate" ]; then portal="$candidate"; break; fi
    done
fi
if [ ! -f "$portal" ]; then
    echo "dc_ui SPA not found; run the ESP-IDF dependency build first" >&2
    exit 1
fi

for route in info state command heartbeat events health; do
    grep -q "\"/api/v2/$route\"" "$httpd" || {
        echo "missing API v2 route: $route" >&2
        exit 1
    }
done

for field in '"schema"' '"product"' '"display_name"'; do
    grep -q "$field" "$httpd" || {
        echo "api v2 info is missing shared-SPA descriptor field: $field" >&2
        exit 1
    }
done

# These product-surface capabilities drive dc_ui's runtime page gating. Keep the
# producer side explicit so the shared SPA path is exercised by DragonBreath.
for capability in power_on auto drying; do
    grep -q "cJSON_CreateString(\"$capability\")" "$httpd" || {
        echo "api v2 info is missing shared-SPA capability: $capability" >&2
        exit 1
    }
done

if grep -Eq '\.uri = "/(status|target|heartbeat|reset)"' "$httpd"; then
    echo "alpha API route was reintroduced" >&2
    exit 1
fi

# Dashboard/control ownership invariants (now in the shared dc_ui SPA, $portal):
grep -q "EventSource('/api/v2/events')" "$portal"    # SSE observer
grep -q "if(polling)" "$portal"                      # serialized poll fallback (no overlapping fetches)
grep -q "lease_id:lease" "$portal"                   # heartbeat sends the exact device-issued lease
grep -q "lease=r.lease_id" "$portal"                 # lease taken only from the command response
grep -q "l.owner!==actor" "$portal"                  # a stale/foreign lease is dropped
grep -q "s.target.maximum_c" "$portal"               # UI honors the runtime max-target ceiling
grep -q 'id="a-range"' "$portal"                     # Automatic: chamber-target control present
grep -q 'id="d-range"' "$portal"                     # Dry: target control present
grep -q 'id="a-action"' "$portal"                    # Automatic: primary action present
grep -q 'id="d-action"' "$portal"                    # Dry: primary action present
grep -q 'id="a-msg"' "$portal"                       # Automatic: command feedback line present
grep -q 'id="d-msg"' "$portal"                       # Dry: command feedback line present
grep -q 'if(ui.schema!=null && ui.schema!==1) return' "$portal" # unknown UI schema degrades safely
grep -q "command('auto', {target_c:fields.autoT.val" "$portal"         # auto sends the user's target+threshold
grep -q "command('drying_start', {target_c:fields.dryT.val" "$portal"  # dry sends the user's target+hours
grep -q "Rejected: '+" "$portal"                     # command rejection surfaced to the user
grep -q 'strcmp(s_replay\[i\].actor_id, actor_id)' "$httpd"

# Remembered mode parameters must stay in the authoritative snapshot and state
# document: the dashboard pre-fills from them and buttons re-arm from them.
grep -q "s.params" "$portal" || {
    echo "dashboard no longer reads the params snapshot" >&2
    exit 1
}
for field in manual_target_c auto_target_c auto_bed_threshold_c dry_target_c dry_hours; do
    grep -q "\"$field\"" "$httpd" || {
        echo "state document is missing params.$field" >&2
        exit 1
    }
    grep -q "pr.$field" "$portal" || {
        echo "dashboard no longer pre-fills from params.$field" >&2
        exit 1
    }
done
grep -q 's->params.manual_target_c' "$httpd" || {
    echo "state document no longer uses the lock-consistent params snapshot" >&2
    exit 1
}
grep -q 'expected->valuedouble < UINT32_MAX' "$httpd"

# Bambu chamber-control diagnostics: the public state must expose the printer
# chamber sample and its age so hardware validation can distinguish live external
# regulation from local-NTC fallback without altering heater behavior.
for field in printer_chamber_temperature_c printer_chamber_age_ms; do
    grep -q "\"$field\"" "$httpd" || {
        echo "state document is missing environment.$field" >&2
        exit 1
    }
done

# Controller-agnostic HMI contract: the product publishes the process variable,
# normalized request/allowed output and one derived constraint. The sparse /diag
# page consumes those fields rather than exposing raw controller internals.
for field in controller preferred_source effective_source process_variable_c controller_request allowed_output constraint; do
    grep -q "\"$field\"" "$httpd" || {
        echo "state document is missing control.loop.$field" >&2
        exit 1
    }
done
for field in control-source target request allowed delivered constraint safety; do
    grep -q "id=\"$field\"" "$diagnostics" || {
        echo "diagnostics instrument panel is missing $field" >&2
        exit 1
    }
done
grep -q 'DBConsequentialToggle.mount' "$diagnostics"
grep -q "'/settings?filter_auto='" "$diagnostics"
grep -q 'serverState' "$consequential_toggle"
grep -q 'input.checked = authoritative.value' "$consequential_toggle"
grep -q "dialog.addEventListener('cancel'" "$consequential_toggle"
grep -q 'document.activeElement !== confirm' "$consequential_toggle"
grep -q '"/ui/consequential-toggle.js"' "$adapter"

# Ownership boundary: core owns HTTP/provisioning/recovery; the product adapter
# supplies API registration, authorization, heater safety and image identity.
grep -q 'dc_portal_start(&cfg)' "$adapter"
grep -q 'pb_httpd_register(server)' "$adapter"
grep -q 'snap.mode == PB_MODE_OFF && !snap.heater_output' "$adapter"
grep -q 'panda_breath' "$adapter"
grep -q '\.uri = "/km-config"' "$adapter" || {
    echo "shipped Klipper-MQTT config generator route is missing" >&2
    exit 1
}
grep -q '########## moonraker.conf ##########' "$adapter"
grep -q '########## printer.cfg ##########' "$adapter"
grep -q 'mosquitto ACL (least privilege)' "$adapter"
grep -q 'httpd.max_uri_handlers = 48' "$adapter" || {
    echo "DragonBreath product routes no longer have a sufficient handler budget" >&2
    exit 1
}
callback_line=$(grep -n 'config->register_product_routes(s_httpd' "$core_portal" | cut -d: -f1)
catchall_line=$(grep -n 'const httpd_uri_t catchall' "$core_portal" | cut -d: -f1)
if [ "$callback_line" -ge "$catchall_line" ]; then
    echo "product routes (including favicon) must register before the SPA catch-all" >&2
    exit 1
fi
plan_line=$(grep -n 'db_portal_plan_product_save' "$adapter" | cut -d: -f1)
first_setter_line=$(grep -n 'dc_moonraker_set_config' "$adapter" | cut -d: -f1)
source_setter_line=$(grep -n 'dc_source_set(plan.source)' "$adapter" | cut -d: -f1)
if [ "$plan_line" -ge "$first_setter_line" ] || [ "$source_setter_line" -le "$first_setter_line" ]; then
    echo "product saves must fully plan before persistence and bind the source last" >&2
    exit 1
fi
grep -q '"/api/v1/provisioning"' "$core_portal"
grep -q '"/api/v1/system/update"' "$core_portal"
grep -q '"/api/v1/system/reset"' "$core_portal"

if grep -q 'cJSON_AddStringToObject(lease, "id"' "$httpd"; then
    echo "unauthenticated state exposes raw lease id" >&2
    exit 1
fi

echo "api v2 static contract checks: PASS"
