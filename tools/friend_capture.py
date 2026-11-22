"""Arm a running Friend sampler through a request file, or inspect saved arguments."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
STATE = 'game_rpc_sample_state.json'
REQUEST = 'rpc_sample_capture.request'
REVISION = 'friend-capture-v2'


def read_state(capture: Path) -> dict:
    value = json.loads((capture / STATE).read_text(encoding='utf-8'))
    if not isinstance(value, dict):
        raise ValueError('Invalid RPC sampler state')
    return value


def latest_capture(root: Path = ROOT / 'out' / 'reflection') -> Path:
    candidates = []
    for path in root.glob('*/' + STATE):
        try:
            state = read_state(path.parent)
            if state.get('sampler_revision') == REVISION and state.get('capture_scope') == 'friends':
                candidates.append(path)
        except (OSError, ValueError):
            continue
    if not candidates:
        raise ValueError('No Friend v2 capture found. Start run_friend_capture.bat first.')
    return max(candidates, key=lambda path: path.stat().st_mtime_ns).parent


def arm_capture(capture: Path, timeout: float = 8) -> dict:
    if not 0 < timeout <= 30:
        raise ValueError('Timeout must be in (0, 30] seconds')
    state = read_state(capture)
    if (state.get('sampler_revision') != REVISION or state.get('capture_scope') != 'friends'
            or state.get('installed') is not True):
        raise ValueError('This capture has no installed Friend v2 sampler; restart with run_friend_capture.bat.')
    if time.time() - (capture / STATE).stat().st_mtime > 15:
        raise ValueError('Sampler heartbeat is stale. The client may have exited or still be writing reflection.')
    generation = state.get('window_id')
    if not isinstance(generation, int) or generation < 0:
        raise ValueError('Sampler window ID is missing')
    request = capture / REQUEST
    # Never overwrite another pending request. No process access or key simulation is needed.
    with request.open('x', encoding='utf-8') as stream:
        stream.write('friend-capture-v2\n')
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            current = read_state(capture)
            if (current.get('window_id', -1) > generation and current.get('arm_source') == 'request_file'
                    and not request.exists()):
                return {'capture': str(capture), 'window_id': current['window_id'],
                        'status': current.get('status'), 'window_seconds': current.get('window_seconds'),
                        'message': 'New window acknowledged; now trigger the friend operation.'}
        except (OSError, ValueError):
            pass  # A live state write may be between truncation and flush; wait for the next heartbeat.
        time.sleep(0.1)
    raise ValueError('No rearm acknowledgement. The request remains pending if not yet consumed; do not treat it as armed.')


def inspect_capture(capture: Path) -> dict:
    state = read_state(capture)
    counts = {'Client_Friend_RPC': 0, 'Server_Friend_RPC': 0}
    custom_types = set()
    truncated = 0
    samples = capture / 'game_rpc_samples.jsonl'
    with samples.open(encoding='utf-8') as stream:
        for number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            row = json.loads(line)
            kind = row.get('kind')
            if kind not in counts:
                continue
            if row.get('module_sha256') != state.get('module_sha256'):
                raise ValueError(f'Sample {number} belongs to a different module')
            if row.get('wire_packet_bytes_captured') is not False:
                raise ValueError(f'Sample {number} is not an argument-only record')
            counts[kind] += 1
            truncated += bool(row.get('payloads_truncated') or row.get('params_truncated'))
            for wrapper in row.get('wrappers', []):
                if wrapper.get('custom_type_name'):
                    custom_types.add(wrapper['custom_type_name'])
    missing = [kind + '_not_observed' for kind, count in counts.items() if not count]
    if truncated:
        missing.append('truncated_payloads')
    return {'capture': str(capture), 'sampler_revision': state.get('sampler_revision'),
            'status': state.get('status'), 'friend_diagnosis': state.get('friend_diagnosis'),
            'friend_samples': counts, 'custom_types': sorted(custom_types), 'truncated_samples': truncated,
            'missing': missing, 'wire_packet_bytes_captured': False, 'packet_mapping_verified': False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('arm', 'inspect'))
    parser.add_argument('capture', type=Path, nargs='?', help='capture directory; defaults to the newest Friend v2 capture')
    args = parser.parse_args()
    try:
        capture = args.capture.resolve() if args.capture else latest_capture()
        result = arm_capture(capture) if args.command == 'arm' else inspect_capture(capture)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (OSError, ValueError) as exc:
        print(f'[ERROR] {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
