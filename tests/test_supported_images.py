"""Tests for the image-extension pre-filter (synthetic names only)."""

from wd_face_recognition import (
    SUPPORTED_IMAGE_EXTENSIONS,
    __version__,
    is_supported_image,
)


def test_common_extensions_accepted():
    for name in ("a.jpg", "b.jpeg", "c.png", "d.heic", "e.tif", "f.tiff"):
        assert is_supported_image(name), name


def test_case_insensitive():
    assert is_supported_image("PHOTO.JPG")
    assert is_supported_image("Scan.HeIC")


def test_unsupported_rejected():
    for name in ("a.cr2", "b.nef", "c.dng", "d.gif", "e.bmp", "f.pdf"):
        assert not is_supported_image(name), name


def test_no_extension_rejected():
    assert not is_supported_image("IMG_0001")
    assert not is_supported_image(".hidden")


def test_directory_path_uses_final_component():
    assert is_supported_image("/shares/Photos/2026/img.png")
    assert not is_supported_image("/shares/Photos/2026/notes.txt")


def test_version_and_extension_set():
    assert __version__ == "0.1.0"
    assert ".jpg" in SUPPORTED_IMAGE_EXTENSIONS
    assert ".cr2" not in SUPPORTED_IMAGE_EXTENSIONS
