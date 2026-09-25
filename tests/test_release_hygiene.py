from __future__ import annotations

from tools.public_release_check import PRIVATE_PATTERNS, check


def test_public_checkout_has_no_private_material() -> None:
    assert check() == []


def test_hygiene_patterns_catch_private_inputs() -> None:
    assert PRIVATE_PATTERNS["private_key"].search(b"-----BEGIN " + b"PRIVATE KEY-----")
    assert PRIVATE_PATTERNS["private_ip"].search(b"192" + b".168.1.50")
    assert PRIVATE_PATTERNS["windows_absolute_path"].search(b"C:" + b"\\Users\\person\\secret.txt")
    assert PRIVATE_PATTERNS["credential_assignment"].search(b"password" + b" = 'example-password'")
