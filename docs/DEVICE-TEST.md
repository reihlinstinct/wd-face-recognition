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
   device it must say so in clear language on the setup screen, AND write the
   same diagnostics into the report JSON. If the app cannot start far enough
   to serve the page, its bootstrap must drop a minimal
   `startup-failure-<date>.json` into `/shares/facerecog-reports/` - retrieve
   it via the dashboard file manager. A silent crash is a FAIL - report it.
4. Open the app page: `https://<nas-ip>:8443/` from the LAN. BEFORE entering
   anything, compare the browser's certificate fingerprint against
   `/shares/facerecog-reports/fingerprint.txt` (readable via the authenticated
   WD dashboard file manager) - enrollment sends the WD ADMIN credentials, so
   the fingerprint must come from that independent channel, not from the page.
   The FIRST-RUN wizard must then load: 1) enter the WD dashboard ADMIN
   credentials (owner check), 2) create the app password, 3) choose folders to
   scan (default: nothing), 4) finish. Not a status page. Use
   `http://<nas-ip>:8088/` ONLY if the LAN is trusted - first-run over HTTP
   sends credentials in cleartext.
5. **Stop** the app from the dashboard, confirm the page stops answering; **Start** again.
6. Run the self-test (section 2) FIRST so a report exists and the face DB is
   NON-EMPTY (the self-test's synthetic faces populate it - that is what the
   retention check needs; if a build cannot populate the DB, defer the
   DB-retention part to a later milestone and say so).
7. **In-place upgrade:** install the NEXT build's `.bin` over the running app
   via the dashboard (no removal). Confirm settings, the face DB and the
   report survive - this exercises the real upgrade path incl. migrations.
8. **Remove** the app; confirm it disappears from the Apps list. The face DB,
   settings and the reports share are kept by design (only "remove data too"
   deletes them).
9. **Reinstall** the same `.bin`; confirm the previous report, the face DB
   records and settings survived. That verifies the retention contract.

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
1. With the app running, FIRST read the certificate fingerprint out of band:
   in the WD dashboard's file manager (authenticated) open
   `/shares/facerecog-reports/fingerprint.txt`. Then open
   `https://<nas-ip>:8443/` and accept the cert warning ONLY if the browser's
   fingerprint matches that file (a fingerprint shown by the page itself proves
   nothing). LOG IN with the password from step 4; then press **Run self-test**.
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
RSS; CPU%/RSS under the 80% cap; HEIC support check; device baseline (step 0);
build flags PROVING the no-AVX build (compiled-in flag list); GeoNames
gazetteer on-disk size and in-RAM lookup cost. A worker network-isolation
check is included too: the test starts a listener the worker COULD otherwise
reach (on the LAN, or 127.0.0.1 in the parent network namespace) and has the
sandboxed worker try to connect - the attempt MUST fail, which can then only
mean the sandbox enforced it (not that nothing was listening).
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
