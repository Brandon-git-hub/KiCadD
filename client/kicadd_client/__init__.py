"""Minimal KiCadD IPC client.

Talks protobuf-over-NNG to `kicad-cli api-server`. Pure IPC: no SWIG, no
`import pcbnew`, nothing installed into the system Python. Run it from an
isolated venv (see KiCadD/README.md).
"""

import os
import sys
import time

import pynng
from google.protobuf import empty_pb2

_PB = os.path.join(os.path.dirname(__file__), "_pb")
if _PB not in sys.path:
    sys.path.insert(0, _PB)

from common import envelope_pb2  # noqa: E402
from common.commands import base_commands_pb2  # noqa: E402
from kicadd import board_pb2 as kd  # noqa: E402

NM_PER_MM = 1_000_000


class ApiError(RuntimeError):
    def __init__(self, status, message):
        super().__init__(f"{envelope_pb2.ApiStatusCode.Name(status)}: {message}")
        self.status = status


class Client:
    def __init__(self, socket, client_name="kicadd-py", timeout_ms=30000):
        self._sock = pynng.Req0(dial=socket, recv_timeout=timeout_ms, send_timeout=timeout_ms)
        self._name = client_name
        self.last_rtt_us = 0

    def close(self):
        self._sock.close()

    def call(self, request, response_type):
        env = envelope_pb2.ApiRequest()
        env.header.client_name = self._name
        env.message.Pack(request)

        t0 = time.perf_counter()
        self._sock.send(env.SerializeToString())
        raw = self._sock.recv()
        self.last_rtt_us = int((time.perf_counter() - t0) * 1e6)

        resp = envelope_pb2.ApiResponse()
        resp.ParseFromString(raw)

        if resp.status.status != envelope_pb2.AS_OK:
            raise ApiError(resp.status.status, resp.status.error_message)

        out = response_type()
        resp.message.Unpack(out)
        return out

    # --- upstream -----------------------------------------------------------
    def ping(self):
        return self.call(base_commands_pb2.Ping(), empty_pb2.Empty)

    # --- KiCadD -------------------------------------------------------------
    def summary(self):
        return self.call(kd.GetBoardSummary(), kd.BoardSummaryResponse)

    def query_radius(self, radius_mm, ref=None, point_mm=None, kinds=(), limit=0, with_ids=False):
        req = kd.QueryRadius(radius=int(radius_mm * NM_PER_MM), limit=limit, with_ids=with_ids)
        if ref is not None:
            req.reference = ref
        else:
            req.point.x = int(point_mm[0] * NM_PER_MM)
            req.point.y = int(point_mm[1] * NM_PER_MM)
        req.kinds.extend(kinds)
        return self.call(req, kd.QueryResponse)

    def query_net(self, net, limit=0):
        return self.call(kd.QueryNet(net=net, limit=limit), kd.QueryResponse)

    def move(self, refs, dx_mm=0.0, dy_mm=0.0, drot_deg=0.0):
        req = kd.MoveDelta(refs=list(refs), dx=int(dx_mm * NM_PER_MM), dy=int(dy_mm * NM_PER_MM),
                           drot_decideg=int(round(drot_deg * 10)))
        return self.call(req, kd.MoveResponse)
