"""Canonicalize, hash, and strictly decode U4 control-plane JSON."""

from __future__ import annotations

import hashlib
import json
import math
from collections.abc import Mapping
from typing import Any

from ...errors import BridgeProtocolError

MAX_CONTROL_PAYLOAD_BYTES = 1 * 1024 * 1024


def canonical_json(value: Any) -> bytes:
    """Serialize a JSON-compatible value using the U4 canonical ordering.

    Args:
        value: A JSON-compatible scalar, list, or string-keyed mapping.

    Returns:
        Canonical UTF-8 JSON bytes with deterministic object-key and number
        normalization suitable for hashing and the control plane.

    Raises:
        BridgeProtocolError: If a number is non-finite or outside the
            interoperable integer range, an object key is not a string, or a
            value type is unsupported.
    """

    def encode(item: Any) -> str:
        """Encode one nested JSON value using canonical scalar and container rules."""

        if item is None:
            return "null"
        if item is True:
            return "true"
        if item is False:
            return "false"
        if isinstance(item, str):
            return json.dumps(item, ensure_ascii=False, separators=(",", ":"))
        if isinstance(item, int):
            if abs(item) > 2**53 - 1:
                raise BridgeProtocolError(
                    "JSON integer exceeds the interoperable range",
                    code="JSON_INTEGER_RANGE",
                )
            return str(item)
        if isinstance(item, float):
            if not math.isfinite(item):
                raise BridgeProtocolError("non-finite JSON number", code="JSON_NON_FINITE")
            if item == 0.0:
                return "0"
            # Normalize through the shortest stable decimal/exponent spelling
            # so equivalent values hash identically across language runtimes.
            raw = repr(item).lower()
            if "e" not in raw:
                return raw[:-2] if raw.endswith(".0") else raw
            mantissa, exponent_text = raw.split("e")
            exponent = int(exponent_text)
            sign = "-" if mantissa.startswith("-") else ""
            digits = mantissa.removeprefix("-").replace(".", "")
            if -6 <= exponent < 21:
                decimal_at = 1 + exponent
                if decimal_at <= 0:
                    return sign + "0." + "0" * (-decimal_at) + digits
                if decimal_at >= len(digits):
                    return sign + digits + "0" * (decimal_at - len(digits))
                return sign + digits[:decimal_at] + "." + digits[decimal_at:]
            return f"{mantissa}e{'+' if exponent >= 0 else ''}{exponent}"
        if isinstance(item, list):
            return "[" + ",".join(encode(element) for element in item) + "]"
        if isinstance(item, Mapping):
            if not all(isinstance(key, str) for key in item):
                raise BridgeProtocolError("JSON object keys must be strings", code="JSON_KEY_TYPE")
            # JCS orders keys by UTF-16 code units, not Python code points.
            ordered = sorted(item, key=lambda key: key.encode("utf-16-be", errors="surrogatepass"))
            return "{" + ",".join(f"{encode(key)}:{encode(item[key])}" for key in ordered) + "}"
        raise BridgeProtocolError(
            f"unsupported JSON value: {type(item).__name__}",
            code="JSON_VALUE_TYPE",
        )

    return encode(value).encode("utf-8")


def sha256_json(value: Any) -> str:
    """Hash one canonical JSON value for the U4 protocol boundary.

    Args:
        value: The JSON-compatible control DTO to canonicalize before hashing.

    Returns:
        A lowercase SHA-256 digest of ``canonical_json(value)``.

    Raises:
        BridgeProtocolError: If canonical serialization rejects ``value``.
    """

    return hashlib.sha256(canonical_json(value)).hexdigest()


def strict_loads(payload: bytes) -> dict[str, Any]:
    """Decode one strict control object received from SocketBridge.

    Args:
        payload: UTF-8 JSON bytes from a bounded control-plane frame.

    Returns:
        A JSON object with unique string keys and interoperable finite numbers.

    Raises:
        BridgeProtocolError: If the payload is oversized, malformed, non-UTF-8,
            not an object, contains duplicate keys, non-finite numbers, or an
            integer outside the interoperable range.
    """

    if len(payload) > MAX_CONTROL_PAYLOAD_BYTES:
        raise BridgeProtocolError("control payload exceeds the limit", code="CONTROL_PAYLOAD_TOO_LARGE")

    def reject_duplicate(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        """Reject duplicate object keys while constructing the decoded mapping."""

        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise BridgeProtocolError(f"duplicate JSON key: {key}", code="JSON_DUPLICATE_KEY")
            result[key] = value
        return result

    def reject_constant(value: str) -> None:
        """Reject a JSON non-finite numeric token."""

        raise BridgeProtocolError(f"non-finite JSON number: {value}", code="JSON_NON_FINITE")

    # Reject duplicate keys and non-finite constants while decoding so invalid
    # control data cannot be normalized into a different accepted object.
    try:
        value = json.loads(
            payload.decode("utf-8", errors="strict"),
            object_pairs_hook=reject_duplicate,
            parse_constant=reject_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise BridgeProtocolError("invalid JSON control payload", code="JSON_DECODE") from exc
    if not isinstance(value, dict):
        raise BridgeProtocolError("control payload must be a JSON object", code="JSON_OBJECT_REQUIRED")

    def validate_numbers(item: Any) -> None:
        """Validate finite numbers recursively inside a decoded JSON object."""

        if isinstance(item, bool) or item is None:
            return
        if isinstance(item, int):
            if abs(item) > 2**53 - 1:
                raise BridgeProtocolError("JSON integer exceeds the interoperable range", code="JSON_INTEGER_RANGE")
            return
        if isinstance(item, float):
            if not math.isfinite(item):
                raise BridgeProtocolError("non-finite JSON number", code="JSON_NON_FINITE")
            return
        if isinstance(item, list):
            for element in item:
                validate_numbers(element)
        elif isinstance(item, dict):
            for element in item.values():
                validate_numbers(element)

    validate_numbers(value)
    return value
