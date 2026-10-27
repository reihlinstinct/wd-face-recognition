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
4. Open the app page: `http://<nas-ip>:8088/` from the LAN - the FIRST-RUN setup/login
   screen must load (password creation), not a status page.
5. **Stop** the app from the dashboard, confirm the page stops answering; **Start** again.
6. **Remove** the app; confirm it disappears from the Apps list. The face DB and
   reports share are kept by design (only "remove data too" deletes them).
   (Reinstall after.)

Each of: install / start / stop / remove is part of M1 acceptance - please report
pass/fail per step, with a photo/screenshot if anything looks wrong.

## 2. Self-test + benchmark (M2 acceptance)
1. With the app running, open `http://<nas-ip>:8088/` and press **Run self-test**.
2. The app benchmarks detection + embedding on built-in synthetic images
   (no real photos are used or scanned) and writes a report to a dedicated
   PRIVATE share created at install:
   `/shares/facerecog-reports/facerecog-selftest-<date>.json` (plus a `.log`).
   NOT the world-readable Public share.
3. Send that `.json` file back (attach it in chat / email).

The report contains: per-photo detection latency (1 and 2 threads), per-face
embedding latency, peak memory, HEIC support check, CPU/RAM as seen on the device.
That is the real-hardware go/no-go data for milestone M2.

## 3. Rules
- Only the synthetic self-test runs at this stage - the indexer does not scan
  anything until folders are chosen in the UI (default: nothing scanned).
- If a step fails, stop there and report the step number + what happened;
  do not retry in a loop.
