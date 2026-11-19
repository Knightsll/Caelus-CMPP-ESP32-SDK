#!/usr/bin/env python3
"""Explicit bench test; credentials stay in the box's private registry.

Requires the official mcp client and websockets installed on the Caelus box.
No model calls, BLE writes, automatic command retries, or configuration changes.
"""
import argparse
import asyncio
import json
import math
import os
from pathlib import Path
import struct
import time
import traceback
from urllib.parse import urlsplit, urlunsplit
from uuid import uuid4
import wave

import httpx2
from mcp import Client
from mcp.client.streamable_http import streamable_http_client
from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosed


async def resource(client, uri):
    result = await client.read_resource(uri)
    texts = [c.text for c in result.contents if hasattr(c, 'text')]
    assert len(texts) == 1, 'Resource not unique'
    return json.loads(texts[0])


async def state(client):
    return (await resource(client, 'cmpp://state'))['capabilities']['voice.channel']


async def execute(client, uid, kind, arguments):
    command_id, trace_id = str(uuid4()), str(uuid4())
    command = dict(schema='cmpp.command.v1', protocol_version='1.0',
        command_id=command_id, trace_id=trace_id,
        target=dict(device_id=uid, capability_id='voice.channel'),
        command_type=kind, requested_by='caelus-hardware-bench',
        reason='Explicit hardware audio and status validation',
        safety_class='reversible', arguments=arguments, ttl_ms=5000,
        idempotency_key=command_id, dry_run=False, requires_ack=True)
    result = await client.call_tool('cmpp.execute', {'command': command})
    receipt = result.structured_content
    assert not result.is_error and isinstance(receipt, dict), 'Command failed'
    assert receipt.get('command_id') == command_id and receipt.get('trace_id') == trace_id
    assert receipt.get('status') == 'completed', 'Command not completed'
    return receipt['result']


async def send(ws, kind, **fields):
    await ws.send(json.dumps(dict(type=kind, **fields), separators=(',', ':')))


async def recv(ws, timeout=10):
    msg = await asyncio.wait_for(ws.recv(), timeout)
    return msg if isinstance(msg, bytes) else json.loads(msg)


async def play(client, ws, pcm):
    turn = str(uuid4())
    await send(ws, 'output.start', turn_id=turn)
    seen_playing = False

    async def monitor():
        nonlocal seen_playing
        while True:
            seen_playing |= bool((await state(client))['playing'])
            await asyncio.sleep(.1)

    monitor_task = asyncio.create_task(monitor())
    try:
        deadline = time.monotonic()
        for offset in range(0, len(pcm), 640):
            frame = pcm[offset:offset + 640]
            await ws.send(frame)
            deadline = max(deadline + len(frame) / 32000, time.monotonic())
            await asyncio.sleep(max(0, deadline - time.monotonic()))
        await send(ws, 'output.end', turn_id=turn)
        assert await recv(ws) == dict(type='output.drained', turn_id=turn), 'No matching drained'
    finally:
        monitor_task.cancel()
        await asyncio.gather(monitor_task, return_exceptions=True)
    s = await state(client)
    assert not s['playing'] and s['awaiting_response'] and not s['recording']
    assert seen_playing, 'Never observed playing state'
    print('playback_pass', dict(bytes=len(pcm), playing_observed=True, drained=True,
                                waiting_until_explicit_listening=True), flush=True)
    await send(ws, 'session.state', state='listening')
    # Ordering barrier: websocket Pong follows processing of the listening frame.
    await (await ws.ping())
    s = await state(client)
    assert s['ready'] and not s['awaiting_response'] and not s['playing']


async def capture(client, ws, destination, timeout):
    print('PTT_READY: release SW2, hold while speaking, then release', flush=True)
    first = await recv(ws, timeout)
    assert isinstance(first, dict) and first.get('type') == 'input.start'
    turn = first['turn_id']
    pcm = bytearray()
    async with asyncio.timeout(timeout):
        while True:
            msg = await recv(ws)
            if isinstance(msg, bytes):
                assert len(msg) == 640, 'Invalid input frame'
                pcm.extend(msg)
                assert len(pcm) <= 32000 * timeout, 'Capture exceeded bound'
            else:
                assert msg == dict(type='input.end', turn_id=turn), 'Missing matching input.end'
                break
    s = await state(client)
    assert not s['recording'] and s['awaiting_response']
    values = struct.unpack('<' + 'h' * (len(pcm) // 2), pcm)
    assert values, 'No samples'
    fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as raw, wave.open(raw, 'wb') as wav:
        wav.setparams((1, 2, 16000, 0, 'NONE', 'not compressed'))
        wav.writeframes(pcm)
    print('capture_pass', dict(bytes=len(pcm), seconds=len(pcm)/32000,
        peak=max(abs(x) for x in values), rms=round(math.sqrt(sum(x*x for x in values)/len(values)), 1),
        input_end=True, awaiting_response=True), flush=True)
    return bytes(pcm)


async def lifecycle(client, connection, uid):
    """No audio frames: test close races and genuine fault/recovery evidence."""
    async def observe_until(predicate):
        async with asyncio.timeout(5):
            while True:
                value = await state(client)
                if predicate(value):
                    return value
                await asyncio.sleep(.05)

    async def opened():
        channel = await execute(client, uid, 'voice.channel.open', {})
        endpoint = urlsplit(connection['url'])
        host = f'[{endpoint.hostname}]' if ':' in endpoint.hostname else endpoint.hostname
        url = urlunsplit(('wss' if endpoint.scheme == 'https' else 'ws',
                         f'{host}:{channel["ws_port"]}', channel['ws_path'], '', ''))
        ws = await connect(url, additional_headers=connection['headers'], proxy=None,
            compression=None, ping_interval=10, ping_timeout=10, close_timeout=2)
        try:
            await send(ws, 'hello', protocol='caelus.voice.v1', channel_id=channel['channel_id'])
            assert await recv(ws) == dict(type='ready', protocol='caelus.voice.v1', channel_id=channel['channel_id'])
        except BaseException:
            await ws.close()
            raise
        return channel, ws

    def stopped(s):
        return not s['channel_id'] and not any(s[k] for k in
            ('ready', 'recording', 'playing', 'awaiting_response'))

    channel, ws = await opened()
    await ws.close()
    s = await observe_until(stopped)
    assert not s['fault'] and s['last_stop_reason'] == 'peer_closed'
    print('normal_ws_close_pass', flush=True)

    for attempt in range(5):
        channel, ws = await opened()

        async def controls():
            try:
                for _ in range(12):
                    await send(ws, 'session.state', state='thinking')
                    await send(ws, 'session.state', state='listening')
                    await asyncio.sleep(.002)
            except ConnectionClosed:
                pass  # Expected when the concurrent MCP close wins.

        async def close_command():
            await asyncio.sleep(.005)
            await execute(client, uid, 'voice.channel.close', {'channel_id': channel['channel_id']})

        try:
            await asyncio.gather(controls(), close_command())
        finally:
            await ws.close()
        s = await observe_until(stopped)
        assert not s['fault'] and s['last_stop_reason'] == 'requested'
        print('mcp_close_interleaved_pass', attempt + 1, flush=True)

    channel, ws = await opened()
    try:
        await send(ws, 'session.state', state='invalid_bench_state')
        try:
            msg = await recv(ws)
            assert isinstance(msg, dict) and msg.get('type') == 'error'
        except ConnectionClosed:
            pass  # Best-effort error frame, closure is authoritative.
    finally:
        await ws.close()
    s = await observe_until(stopped)
    assert s['fault'] and s['last_fault_reason'] == s['last_stop_reason'] == 'invalid_frame'
    print('genuine_fault_preserved_pass', flush=True)

    channel, ws = await opened()
    try:
        s = await state(client)
        assert s['ready'] and not s['fault'] and s['last_fault_reason'] == 'invalid_frame'
        print('hello_recovers_fault_retains_history_pass', flush=True)
    finally:
        await ws.close()
    s = await observe_until(stopped)
    assert not s['fault'] and s['last_stop_reason'] == 'peer_closed'
    print('lifecycle_released', flush=True)


async def main(args):
    registry = json.loads(args.registry.expanduser().read_text())
    record = registry['devices'][args.uid]
    assert record['pairing']['state'] == 'completed'
    connection = record['connection']
    assert connection['device_uid'] == args.uid
    async with httpx2.AsyncClient(headers=connection['headers'], trust_env=False,
                                 timeout=10, follow_redirects=False) as http:
        transport = streamable_http_client(connection['url'], http_client=http)
        async with Client(transport, mode='legacy', cache=None, read_timeout_seconds=10) as client:
            manifest = await resource(client, 'cmpp://manifest')
            assert manifest['device_id'] == args.uid
            assert manifest['profile_id'] == 'caelus.voice-terminal.v1'
            print('identity_verified', args.uid, 'state', await state(client), flush=True)
            if args.mode == 'inspect':
                return
            if args.mode == 'lifecycle':
                await lifecycle(client, connection, args.uid)
                return
            channel = await execute(client, args.uid, 'voice.channel.open', {})
            assert (channel['format'], channel['sample_rate'], channel['channels'],
                    channel['frame_ms'], channel['ws_path'], channel['ws_port']) == (
                    'pcm_s16le', 16000, 1, 20, '/voice', 8081)
            endpoint = urlsplit(connection['url'])
            url = urlunsplit(('wss' if endpoint.scheme == 'https' else 'ws',
                f'{endpoint.hostname}:8081', '/voice', '', ''))
            body_failed = True
            try:
                async with connect(url, additional_headers=connection['headers'], proxy=None,
                        compression=None, ping_interval=10, ping_timeout=10, close_timeout=2,
                        open_timeout=10, max_size=4096, max_queue=16) as ws:
                    await send(ws, 'hello', protocol='caelus.voice.v1', channel_id=channel['channel_id'])
                    assert await recv(ws) == dict(type='ready', protocol='caelus.voice.v1', channel_id=channel['channel_id'])
                    print('media_ready', flush=True)
                    if args.mode == 'record':
                        pcm = await capture(client, ws, args.output.expanduser(), args.timeout)
                    else:
                        # 1.2 s, -24 dBFS peak test tone plus a short final frame.
                        samples = 19300
                        pcm = b''.join(struct.pack('<h', int(2000 * math.sin(2*math.pi*440*i/16000)
                            * min(1, i/320, (samples-1-i)/320))) for i in range(samples))
                    await play(client, ws, pcm)
                    print('listening_restored', flush=True)
                body_failed = False
            finally:
                # close receipt is also valid after WS disconnect cleared this channel.
                try:
                    await execute(client, args.uid, 'voice.channel.close', {'channel_id': channel['channel_id']})
                except Exception:
                    print('close_unconfirmed: verify state using a fresh MCP session', flush=True)
                    if not body_failed:
                        raise
            s = await state(client)
            assert not any(s[k] for k in ('ready', 'recording', 'playing', 'awaiting_response'))
            print('disconnect_cleanup_pass', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--registry', type=Path, required=True)
    parser.add_argument('--uid', required=True)
    parser.add_argument('--mode', choices=('inspect', 'play', 'record', 'lifecycle'), default='inspect')
    parser.add_argument('--timeout', type=int, default=60)
    parser.add_argument('--output', type=Path)
    options = parser.parse_args()
    if options.mode == 'record' and not options.output:
        parser.error('record needs a new private --output WAV path')
    os.umask(0o077)
    try:
        asyncio.run(main(options))
    except Exception as exc:
        # Exception objects from transports can include request details; no dumps.
        def report(error):
            if isinstance(error, BaseExceptionGroup):
                for child in error.exceptions:
                    report(child)
            else:
                frames = traceback.extract_tb(error.__traceback__)
                print('bench_failed', type(error).__name__,
                      [(Path(f.filename).name, f.lineno, f.name) for f in frames[-4:]], flush=True)
        report(exc)
        raise SystemExit(1)
