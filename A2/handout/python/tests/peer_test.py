"""Run from python/tests after building the C peer with make in src."""
import hashlib
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
C_PEER = ROOT / 'src' / 'peer'
PYTHON = ROOT / 'python'
IP = '127.0.0.1'
PASSWORD = 'test'
SIGNATURE = hashlib.sha256(b'test0123456789ABCDEF').digest()

def send(peer, text):
    peer.stdin.write(text + '\n')
    peer.stdin.flush()

def start_peer(command, port, directory):
    peer = subprocess.Popen(command + [IP, str(port)], cwd=directory,
                            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, text=True)
    send(peer, PASSWORD)
    return peer

def register(peer, port):
    send(peer, IP)
    send(peer, str(port))
    time.sleep(1)

def wait_for_file(path, expected):
    for i in range(50):
        if path.is_file() and path.read_bytes() == expected:
            return True
        time.sleep(0.1)
    return False

def request(port, sender, command, body=b'', signature=SIGNATURE):
    header = struct.pack('!16sI32sII', IP.encode(), sender,
                         signature, command, len(body))
    with socket.create_connection((IP, port), timeout=5) as connection:
        connection.sendall(header + body)
        connection.shutdown(socket.SHUT_WR)
        reply = b''
        while True:
            data = connection.recv(8196)
            if not data:
                break
            reply += data
    if len(reply) < 80:
        return None, b''
    length, status, number, count = struct.unpack('!IIII', reply[:16])
    # These direct tests only expect single-block responses.
    payload = reply[80:]
    if number != 0 or count != 1 or length != len(payload):
        return None, b''
    digest = hashlib.sha256(payload).digest()
    if reply[16:48] != digest or reply[48:80] != digest:
        return None, b''
    return status, payload

def main():
    if not C_PEER.is_file():
        print('Build the C peer first: ' + str(C_PEER))
        return 1

    results = []
    peers = []

    def check(name, passed):
        results.append(bool(passed))
        print(('PASS: ' if passed else 'FAIL: ') + name)

    with tempfile.TemporaryDirectory() as directory:
        a = Path(directory) / 'first'
        b = Path(directory) / 'c'
        c = Path(directory) / 'second'
        for folder in (a, b, c):
            folder.mkdir()
        for name in ('tiny.txt', 'hamlet.txt'):
            shutil.copy(PYTHON / 'first_peer' / name, a / name)
        (b / 'binary.bin').write_bytes(bytes(range(256)) * 80)
        (a / 'empty.txt').write_bytes(b'')
        try:
            first = start_peer([sys.executable, str(PYTHON / 'first_peer' / 'peer.py')], 12345, a)
            peers.append(first)
            middle = start_peer([str(C_PEER)], 12346, b)
            peers.append(middle)
            time.sleep(1)
            register(middle, 12345)
            # The initial Python peer omits its own credentials from its reply.
            # Register it with C to complete the network in both directions.
            register(first, 12346)
            for name in ('tiny.txt', 'hamlet.txt', 'empty.txt'):
                send(middle, name)
                check('C retrieves ' + name, wait_for_file(b / name, (a / name).read_bytes()))
            (b / 'from_c.txt').write_bytes(b'File from C\n' * 1000)
            send(first, 'from_c.txt')
            check('Python retrieves from C', wait_for_file(a / 'from_c.txt', (b / 'from_c.txt').read_bytes()))
            send(first, 'binary.bin')
            check('Python retrieves binary.bin from C',
                wait_for_file(a / 'binary.bin',
                                (b / 'binary.bin').read_bytes()))

            second = start_peer([sys.executable, str(PYTHON / 'second_peer' / 'peer.py')], 12347, c)
            peers.append(second)
            time.sleep(1)
            register(second, 12346)
            # Registering with C again reveals whether C learned all three peers.
            status, records = request(12346, 12346, 1)
            check('Three peers, no duplicate registration', status == 2 and len(records) == 3 * 68)
            # Both possible servers have identical files, so random choice is fine.
            data = b'Concurrent transfer\n' * 50000
            (a / 'parallel.txt').write_bytes(data)
            (b / 'parallel.txt').write_bytes(data)
            # Give the second Python peer the same file for the first peer's request.
            (c / 'other.txt').write_bytes(data)
            (b / 'other.txt').write_bytes(data)
            send(first, 'other.txt')
            send(second, 'parallel.txt')
            check('Concurrent retrieval by first peer', wait_for_file(a / 'other.txt', data))
            check('Concurrent retrieval by second peer', wait_for_file(c / 'parallel.txt', data))
            # Check that the first Python peer learned about the second via INFORM.
            status, records = request(12345, 12345, 1)
            check('INFORM reaches first peer', status == 2 and len(records) == 3 * 68)

            check('Unknown command', request(12346, 12346, 9)[0] == 7)
            check('Missing file', request(12346, 12346, 2, b'missing.txt')[0] == 5)
            check('Wrong signature', request(12346, 12346, 2, b'tiny.txt', b'x' * 32)[0] == 4)
            check('Unknown peer', request(12346, 12349, 2, b'tiny.txt')[0] == 3)
            check('Invalid registration body', request(12346, 12346, 1, b'bad')[0] == 7)
        except (OSError, ValueError) as error:
            check('Test execution: ' + str(error), False)
        finally:
            for peer in peers:
                peer.terminate()
            for peer in peers:
                peer.wait()

    print(f'{sum(results)}/{len(results)} tests passed')
    return 0 if results and all(results) else 1

if __name__ == '__main__':
    sys.exit(main())