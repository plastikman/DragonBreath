# RFC: Stock partition layout as canonical + app-only releases (the 1.0 model)

Status: **Draft / design — for review.** Make DragonBreath a stock-layout app that
installs and reverts entirely through the stock firmware's own OTA, ship **only the
app image**, and keep the full-image/USB machinery in the repo for recovery only.

## Why (recap of the reversibility analysis)

Reverting to Panda over WiFi (v1.0.0-rc1's `/update` accepting `panda_breath` images)
only works cleanly when the **stock bootloader, partition table, and spiffs/nvs data
are still on the chip** — i.e. when DragonBreath was installed via stock's OTA and
lives in a stock app slot. A USB full-flash wipes all of that, so it can't cleanly
revert. The fix isn't "match the layout and keep full-flashing" — it's **never
replace the stock bootloader/spiffs at all**: ship DragonBreath as an app that runs
inside the stock partition environment.

This also means we **stop distributing (and stop replacing BIQU's) bootloader +
partition table** — a lighter footprint and a cleaner story.

## The change

### 1. Canonical partition table = stock layout
Replace `partitions.csv` with the stock geometry (decoded from the 1.0.3 image):

```
# was (native)                             # now (stock-matching)
nvs      nvs   0x9000  0x6000              nvs      nvs      0x9000  0x5000 (20K)
otadata  ota   0xf000  0x2000              otadata  ota      0xe000  0x2000
phy_init phy   0x11000 0x1000       →      (dropped — PHY is embedded in the app)
ota_0    app   0x20000 0x1D0000            ota_0    app      0x10000 0x1E0000 (1920K)
ota_1    app   0x1F0000 0x1D0000           ota_1    app      0x1f0000 0x1E0000 (1920K)
                                           spiffs   spiffs   0x3d0000 0x2F000 (188K)
                                           coredump coredump 0x3ff000 0x1000
```

Our app (~1.05 MB) fits the 1920 K slots. We don't use `spiffs`, but keep it at
stock's offset so the layout is byte-for-byte stock (so a reverted `panda_breath`
app finds its assets region). PHY stays embedded (already the case). Keep the table
MD5 (stock uses it).

### 2. Releases ship the app OTA image only
- **Publish:** `dragonbreath-vX.Y.Z.bin` (the app), `manifest.json`, `SHA256SUMS.txt`.
- **Stop publishing:** `-factory.bin` and `-install-bundle.zip`.
- **Keep in the repo:** the full-image build (bootloader + table + app), `tools/flash.py`,
  and the factory/bundle machinery — for **recovery/dev only**, just not attached to
  releases. (Anyone can still `idf.py build` a factory image locally.)

Rationale: with app-only-over-stock-OTA as the one supported install, the factory
image and USB flasher are no longer the product — they're the safety net.

### 3. Install & revert (the only supported flows)
- **Install:** stock web UI → Firmware Update → upload `dragonbreath-vX.Y.Z.bin`
  → boots under the stock bootloader in a stock slot. (Validated on hardware with
  v0.8.0.)
- **Revert:** DragonBreath web UI → `/update` → upload the stock **app** image from
  your backup → back to Panda. (v1.0.0-rc1; stock bootloader/spiffs intact → clean.)
- **Recovery (rare):** USB `tools/flash.py --restore <backup>` — unchanged.

## Alpha/beta tester migration (important — do before 1.0)

Most alpha/beta units were **USB-flashed in the native layout** this session, so they
can't cleanly OTA-upgrade to a stock-layout 1.0 app. **Strong recommendation, to
eliminate layout variability:**

1. **Revert to your stock backup first** — `tools/flash.py --restore <your-stock-backup.bin>`
   over USB (the one USB step). You're back on clean stock.
2. **Install 1.0 via stock's own Firmware Update** — upload `dragonbreath-v1.0.0.bin`.

Now you're on the same stock-layout footing as every new user, and every future
update/revert is app-only over WiFi. (Skipping step 1 and trying to OTA 1.0 onto a
native-layout install will land you in a mismatched partition state.)

## Version-compat checklist (before shipping 1.0 final)
- Layout **confirmed identical on stock 1.0.3 AND 1.0.4** (decoded both backups:
  same nvs/otadata/ota_0/ota_1/spiffs/coredump offsets + sizes). Coupling risk is
  minimal. Re-check on any future stock that changes slot geometry.
- Confirm a first boot on a stock-populated NVS is clean (ignore unknown stock keys).
- Confirm the stock bootloader boots the 1.0 app on both 1.0.3 and 1.0.4.

## Tradeoffs
- **Pro:** one image; clean WiFi install *and* revert; we don't ship/replace BIQU's
  bootloader; simpler releases; smaller attack/impact surface.
- **Con:** DragonBreath is now **coupled to the stock bootloader + partition scheme**
  — a future stock layout change could require an app that still fits it (hence the
  version-compat check). We inherit stock's 20 K nvs (fine) and carry an unused spiffs.
- **Inherent:** a USB full-flash still wipes stock spiffs/bootloader, so USB-installed
  units still can't cleanly revert — which is why USB is demoted to recovery only.

## Implementation checklist (once approved)
1. Swap `partitions.csv` → stock layout; drop `phy_init`; rebuild; host tests + CI green.
2. Re-validate on hardware: stock-OTA install of the new app, then a revert pass
   (upload a stock `panda_breath` app back).
3. `release.yml`: publish only the app image + manifest + sums; stop attaching
   factory/bundle (keep the build steps/machinery, just don't upload).
4. Docs: README/FEATURES — "install via stock's updater; USB is recovery-only";
   add the tester-migration note; update the SAFETY/partition notes.
5. Cut **v1.0.0** (final) once 1.0.4 layout is verified.

## Out of scope
- Shipping any Panda image (unchanged hard line — backups are user-generated).
- Removing the USB flasher / factory build from the repo (we keep them).

Related: `plans/webflash-installer.md` (#53), which this supersedes on the install
model (app-only, no separate installer needed for install; the backup-tool idea is
now optional since revert is a plain `/update`).
