# Device test run sheet (PR4100)

> **בקצרה:** מתקינים את קובץ ה-`.bin` דרך לוח הבקרה של ה-NAS, מריצים "בדיקה עצמית"
> מתוך דף האפליקציה, ושולחים בחזרה את קובץ הדוח שנוצר בשיתוף הפרטי facerecog-reports.
> אין צורך ב-SSH ושום קובץ מקורי לא משתנה.

No SSH needed. No original files are touched. Everything runs on the NAS itself;
results come back as one report file.

## 1. Install (M1 acceptance)
1. Download the `.bin` artifact from the CI run / release page.
2. PR4100 dashboard -> **Apps** -> **Install an app manually** -> pick the `.bin`.
3. Confirm the app appears in the Apps list as **facerecog** and shows *Running*.
   The app runs a STARTUP SELF-CHECK: if any library/symbol is missing on the
   device it must say so in clear language on the setup screen (a silent crash
   is a FAIL - report it).
4. Open the app page: `https://<nas-ip>:8443/` from the LAN - the FIRST-RUN
   wizard must load: 1) enter the WD dashboard ADMIN credentials (owner check),
   2) create the app password, 3) choose folders to scan (default: nothing),
   4) finish. Not a status page. Use `http://<nas-ip>:8088/` ONLY if the LAN is
   trusted - first-run over HTTP sends the password in cleartext.
5. **Stop** the app from the dashboard, confirm the page stops answering; **Start** again.
6. Run the self-test (section 2) FIRST so a report exists. Then **Remove** the
   app; confirm it disappears from the Apps list. The face DB, settings and the
   reports share are kept by design (only "remove data too" deletes them).
7. **Reinstall** the same `.bin`; confirm the previous report is still in
   `/shares/facerecog-reports/` and settings survived. That verifies the
   retention contract.

Each of: install / start / stop / remove is part of M1 acceptance - please report
pass/fail per step, with a photo/screenshot if anything looks wrong.

## 2. System report + self-test + benchmark (M2 acceptance)
0. **System report first:** the self-test records the device's exact baseline:
   firmware/OS version (dashboard version string + `/etc/version`), kernel
   (`uname -a`), glibc version, CPU flags (`cat /proc/cpuinfo`), RAM
   (`/proc/meminfo`) and the installed system libraries. This goes into the
   same report JSON - it confirms or corrects the Debian Bullseye / static-
   binary assumptions the build relies on. Please send it even if nothing
   else works.
1. With the app running, open `https://<nas-ip>:8443/` (accept the cert warning
   ONLY after checking the fingerprint shown on the setup screen) and LOG IN
   with the password from step 4; then press **Run self-test**.
   (`http://<nas-ip>:8088/` instead only if you explicitly enabled HTTP on a
   LAN you trust - the password crosses it in cleartext.)
2. The app benchmarks detection + embedding on built-in synthetic images
   (no real photos are used or scanned) and writes a report to a dedicated
   PRIVATE share created at install:
   `/shares/facerecog-reports/facerecog-selftest-<date>.json` (plus a `.log`).
   NOT the world-readable Public share.
3. Send that `.json` file back (attach it in chat / email).

The report contains: per-photo latency INCLUDING decode, on the PRODUCTION
thread setting (2 threads) and on 1 thread; per-face embedding latency; peak
RSS; CPU%/RSS under the 80% cap; HEIC support check; device baseline (step 0).
Test images: synthetic 12 MP JPEGs (real-photo resolution - small images would
understate decode cost). Pass/fail thresholds (from the design budgets):
detection+decode <= 2 s/photo at 2 threads, peak RSS <= 1.5 GB, UI backend
idle <= 256 MB, startup <= 30 s. These values are IN the JSON. That is the
real-hardware go/no-go data for milestone M2.

## 3. Rules
- Only the synthetic self-test runs at this stage - the indexer does not scan
  anything until folders are chosen in the UI (default: nothing scanned).
- If a step fails, stop there and report the step number + what happened;
  do not retry in a loop.
