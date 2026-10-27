"""On-device face recognition for the WD My Cloud PR4100 (OS 5 app)."""

from pathlib import Path

__version__ = "0.1.0"

# Image discovery pre-filter (docs/DESIGN.md): extensions screened first,
# then confirmed with a magic-byte check at decode time.
SUPPORTED_IMAGE_EXTENSIONS = frozenset(
    {".jpg", ".jpeg", ".png", ".heic", ".tif", ".tiff"}
)


def is_supported_image(filename: str) -> bool:
    """Return True if *filename* ends in a supported image extension.

    Case-insensitive; name check only - the decoder re-verifies content.
    """
    return Path(filename).suffix.lower() in SUPPORTED_IMAGE_EXTENSIONS
