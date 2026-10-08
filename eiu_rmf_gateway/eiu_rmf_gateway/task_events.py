"""WebSocket server for the task events a fleet adapter sends to its ui_websocket_uri."""

import json
import logging

try:
    from websockets.asyncio.server import serve
except ImportError:
    from websockets import serve

from .core import Recorder

log = logging.getLogger('eiu_rmf_gateway')


async def serve_task_events(recorder: Recorder, host: str, port: int):
    async def handler(ws, *_path):
        log.info('fleet adapter connected to the task events socket')
        async for message in ws:
            try:
                envelope = json.loads(message)
            except ValueError:
                continue
            if isinstance(envelope, dict) and envelope.get('type') == 'task_state_update' \
                    and isinstance(envelope.get('data'), dict):
                recorder.add_event('task_state', {'state': envelope['data']})
        log.info('fleet adapter left the task events socket')

    return await serve(handler, host, port)
