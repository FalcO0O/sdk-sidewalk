#!/usr/bin/env python3
# Copyright (c) 2026 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

"""Local map of Amazon Sidewalk device positions.

Reads the last known position of each device with GetResourcePosition, then
subscribes to the MQTT topic that the position destination rule republishes to,
and serves a map on 127.0.0.1. Credentials come from the AWS SDK default chain,
for example a profile set up with `aws sso login`. Nothing is written to disk.
"""

from __future__ import annotations

import argparse
import collections
import json
import sys
import threading
import uuid

from datetime import datetime, timezone
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

STATIC_DIR = Path(__file__).resolve().parent / "static"
CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".png": "image/png",
}
# Map tiles are the only resource loaded from outside the laptop.
CONTENT_SECURITY_POLICY = (
    "default-src 'self'; img-src 'self' data: https://tile.openstreetmap.org; "
    "connect-src 'self'; frame-ancestors 'none'"
)

DEVICE_ID_KEYS = ("wirelessdeviceid", "resourceidentifier", "deviceid")
METHOD_KEYS = ("measurementtype", "solvertype", "solverprovider", "source")
ACCURACY_KEYS = ("horizontalaccuracy", "accuracy")
TIMESTAMP_KEYS = ("timestamp", "time")


def _find_key(data: dict, keys: tuple[str, ...]) -> Any:
    lowered = {k.lower(): v for k, v in data.items() if isinstance(k, str)}
    for key in keys:
        if key in lowered:
            return lowered[key]
    return None


def _is_coordinates(value: Any) -> bool:
    return (
        isinstance(value, list)
        and len(value) >= 2
        and all(isinstance(v, (int, float)) for v in value[:2])
    )


def _walk(value: Any):
    """Yield every dict in a JSON value, including JSON documents stored as strings."""
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except ValueError:
            return
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from _walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from _walk(child)


def parse_position(payload: Any, device_id: str | None = None) -> dict | None:
    """Extract one position from a GeoJSON document or a rule message wrapping it.

    The format of the position destination message is not fixed here: the first
    object with GeoJSON coordinates is used, and the device id, method, accuracy
    and timestamp are looked up in that object, its properties, and the
    enclosing objects.
    """
    objects = list(_walk(payload))
    geo = next((o for o in objects if _is_coordinates(o.get("coordinates"))), None)
    if geo is None:
        return None

    lookup = [geo.get("properties") or {}, geo, *objects]

    def first(keys: tuple[str, ...]) -> Any:
        for obj in lookup:
            if isinstance(obj, dict):
                value = _find_key(obj, keys)
                if value not in (None, ""):
                    return value
        return None

    lon, lat = geo["coordinates"][0], geo["coordinates"][1]
    accuracy = first(ACCURACY_KEYS)
    return {
        "device_id": device_id or first(DEVICE_ID_KEYS),
        "lat": lat,
        "lon": lon,
        "accuracy_m": accuracy if isinstance(accuracy, (int, float)) else None,
        "method": first(METHOD_KEYS),
        "timestamp": first(TIMESTAMP_KEYS),
        "received": datetime.now(timezone.utc).isoformat(timespec="seconds"),
    }


class PositionStore:
    """Recent positions per device, kept in memory only."""

    def __init__(self, history: int):
        self._lock = threading.Lock()
        self._history = history
        self._positions: dict[str, collections.deque] = {}
        self._names: dict[str, str] = {}

    def set_name(self, device_id: str, name: str) -> None:
        with self._lock:
            self._names[device_id] = name

    def add(self, position: dict) -> None:
        device_id = position.get("device_id") or "unknown"
        with self._lock:
            entries = self._positions.setdefault(
                device_id, collections.deque(maxlen=self._history)
            )
            entries.append(position)

    def snapshot(self) -> dict:
        with self._lock:
            ids = set(self._positions) | set(self._names)
            return {
                "devices": [
                    {
                        "id": device_id,
                        "name": self._names.get(device_id, device_id),
                        "positions": list(self._positions.get(device_id, [])),
                    }
                    for device_id in sorted(ids)
                ]
            }


def make_handler(store: PositionStore, port: int):
    allowed_hosts = {f"127.0.0.1:{port}", f"localhost:{port}"}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def _send(self, status: HTTPStatus, body: bytes, content_type: str) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", CONTENT_SECURITY_POLICY)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            # Reject other Host names, so a web page cannot reach the viewer
            # through DNS rebinding.
            if self.headers.get("Host") not in allowed_hosts:
                self._send(HTTPStatus.FORBIDDEN, b"forbidden", "text/plain")
                return

            path = self.path.split("?", 1)[0]
            if path == "/api/positions":
                body = json.dumps(store.snapshot()).encode()
                self._send(HTTPStatus.OK, body, "application/json")
                return

            if path == "/":
                path = "/index.html"
            file = (STATIC_DIR / path.lstrip("/")).resolve()
            if (
                STATIC_DIR not in file.parents
                or not file.is_file()
                or file.suffix not in CONTENT_TYPES
            ):
                self._send(HTTPStatus.NOT_FOUND, b"not found", "text/plain")
                return
            self._send(HTTPStatus.OK, file.read_bytes(), CONTENT_TYPES[file.suffix])

    return Handler


def list_sidewalk_devices(iotwireless) -> dict[str, str]:
    devices: dict[str, str] = {}
    kwargs: dict[str, Any] = {"WirelessDeviceType": "Sidewalk"}
    while True:
        response = iotwireless.list_wireless_devices(**kwargs)
        for device in response.get("WirelessDeviceList", []):
            devices[device["Id"]] = device.get("Name") or device["Id"]
        token = response.get("NextToken")
        if not token:
            return devices
        kwargs["NextToken"] = token


def load_last_positions(iotwireless, store: PositionStore, devices: dict[str, str]) -> int:
    from botocore.exceptions import ClientError

    shown = 0
    for device_id, name in devices.items():
        try:
            response = iotwireless.get_resource_position(
                ResourceIdentifier=device_id, ResourceType="WirelessDevice"
            )
        except ClientError as exc:
            # The policy allows reading only the devices tagged for the demo
            if exc.response.get("Error", {}).get("Code") != "AccessDeniedException":
                store.set_name(device_id, name)
                shown += 1
                print(f"{name}: no position ({exc.response['Error'].get('Code')})")
            continue
        store.set_name(device_id, name)
        shown += 1
        body = response.get("GeoJsonPayload")
        raw = body.read() if hasattr(body, "read") else body
        if not raw:
            print(f"{name}: no position yet")
            continue
        position = parse_position(raw.decode(), device_id)
        if position:
            store.add(position)
            print(f"{name}: last position {position['lat']:.5f}, {position['lon']:.5f}")
    return shown


def connect_mqtt(session, region: str, endpoint: str, topic: str, store: PositionStore):
    from awscrt import auth, mqtt
    from awsiot import mqtt_connection_builder

    def get_credentials():
        creds = session.get_credentials().get_frozen_credentials()
        return auth.AwsCredentials(creds.access_key, creds.secret_key, creds.token)

    def on_message(topic, payload, **kwargs):
        position = parse_position(payload.decode(errors="replace"))
        if position is None:
            print(f"Message on {topic} without coordinates, ignored")
            return
        store.add(position)
        print(
            f"{position['device_id']}: {position['lat']:.5f}, {position['lon']:.5f} "
            f"({position['method'] or 'unknown method'})"
        )

    connection = mqtt_connection_builder.websockets_with_default_aws_signing(
        endpoint=endpoint,
        region=region,
        credentials_provider=auth.AwsCredentialsProvider.new_delegate(get_credentials),
        client_id=f"location-viewer-{uuid.uuid4().hex[:12]}",
        clean_session=True,
        keep_alive_secs=30,
    )
    connection.connect().result(timeout=20)
    future, _ = connection.subscribe(
        topic=topic, qos=mqtt.QoS.AT_LEAST_ONCE, callback=on_message
    )
    future.result(timeout=20)
    print(f"Subscribed to {topic}")
    return connection


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--profile", help="AWS profile, for example the SSO profile")
    parser.add_argument("--region", help="AWS region (default: from the profile)")
    parser.add_argument(
        "--topic",
        default="sidewalk/location-demo/#",
        help='MQTT topic of the position rule (default: "sidewalk/location-demo/#")',
    )
    parser.add_argument(
        "--endpoint",
        help="AWS IoT data endpoint (default: looked up with DescribeEndpoint)",
    )
    parser.add_argument(
        "--device",
        action="append",
        metavar="ID",
        help="wireless device id to show (default: all devices the policy allows reading)",
    )
    parser.add_argument("--port", type=int, default=8000, help="local port (default: 8000)")
    parser.add_argument(
        "--history", type=int, default=50, help="positions kept per device (default: 50)"
    )
    args = parser.parse_args()

    try:
        import boto3
        from botocore.exceptions import BotoCoreError, ClientError
    except ImportError:
        sys.exit("Install the dependencies first: pip install -r requirements.txt")

    session = boto3.Session(profile_name=args.profile, region_name=args.region)
    if session.get_credentials() is None:
        sys.exit("No AWS credentials, log in first: aws sso login --profile <profile>")
    region = session.region_name
    if not region:
        sys.exit("No AWS region, pass --region or set it in the profile")

    store = PositionStore(args.history)
    try:
        iotwireless = session.client("iotwireless")
        if args.device:
            devices = {device_id: device_id for device_id in args.device}
        else:
            devices = list_sidewalk_devices(iotwireless)
        shown = load_last_positions(iotwireless, store, devices)
        print(f"Showing {shown} of {len(devices)} Sidewalk devices, the policy allows reading these")

        endpoint = args.endpoint or session.client("iot").describe_endpoint(
            endpointType="iot:Data-ATS"
        )["endpointAddress"]
    except (BotoCoreError, ClientError) as exc:
        sys.exit(f"AWS request failed: {exc}")

    connection = connect_mqtt(session, region, endpoint, args.topic, store)

    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(store, args.port))
    print(f"Map: http://127.0.0.1:{args.port}/ (Ctrl+C to stop)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        connection.disconnect().result(timeout=5)


if __name__ == "__main__":
    main()
