# wdf_demo - first visible end-to-end slice (issue #18)

Takes a folder of JPEG photos (scanned recursively: subfolders included,
never following symlinks, capped at 8 levels, 1000 photos and a 200k-entry work budget per run so
a full library can't hog the NAS), detects faces (SCRFD-500M), embeds them
(MobileFaceNet ArcFace), clusters by cosine similarity (crude threshold,
union-find), and writes `out/index.html`: a mobile-friendly contact sheet of
face crops grouped by person.

Demo scope: JPEG only (HEIC/PNG tracked separately), no alignment, no naming. The pipeline pieces are
adapted from the M2 ncnn spike (`bench/ncnn`, issue #3) and fold into the
real indexer in M3.

## Build

```sh
bench/ncnn/build-ncnn.sh /tmp/ncnn-build          # pinned ncnn, static, no AVX
bench/ncnn/get-models.sh /tmp/wdf-models /tmp/ncnn-build/install
cmake -S demo -B build/demo -DCMAKE_BUILD_TYPE=Release \
  -DNCNN_INSTALL_DIR=/tmp/ncnn-build/install
cmake --build build/demo --parallel
```

## Run

```sh
build/demo/wdf_demo <photos_dir> /tmp/wdf-models /tmp/wdf-demo-out 2
xdg-open /tmp/wdf-demo-out/index.html   # or copy the folder to your phone
```
