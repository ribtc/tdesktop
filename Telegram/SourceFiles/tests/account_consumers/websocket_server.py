import base64
import hashlib
import socket
import ssl
import struct
import sys
import threading


def read_exact(connection, size):
    result = b""
    while len(result) < size:
        part = connection.recv(size - len(result))
        if not part:
            raise EOFError()
        result += part
    return result


def serve(connection, tls):
    try:
        version, count = read_exact(connection, 2)
        assert version == 5
        assert 0 in read_exact(connection, count)
        connection.sendall(b"\x05\x00")
        assert read_exact(connection, 4) == b"\x05\x01\x00\x03"
        host = read_exact(connection, read_exact(connection, 1)[0])
        assert host == b"target.invalid"
        assert read_exact(connection, 2) == struct.pack("!H", 443)
        connection.sendall(b"\x05\x00\x00\x01\x7f\x00\x00\x01\x01\xbb")
        connection = tls.wrap_socket(connection, server_side=True)
        request = b""
        while not request.endswith(b"\r\n\r\n"):
            request += read_exact(connection, 1)
        headers = dict(line.split(b": ", 1) for line in request.split(b"\r\n")[1:-2])
        accept = base64.b64encode(hashlib.sha1(
            headers[b"Sec-WebSocket-Key"] + b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
        ).digest())
        connection.sendall(b"HTTP/1.1 101 Switching Protocols\r\n"
                           b"Upgrade: websocket\r\nConnection: Upgrade\r\n"
                           b"Sec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        while True:
            opcode, size = read_exact(connection, 2)
            assert size & 128
            size &= 127
            if size == 126:
                size = struct.unpack("!H", read_exact(connection, 2))[0]
            elif size == 127:
                size = struct.unpack("!Q", read_exact(connection, 8))[0]
            mask = read_exact(connection, 4)
            payload = bytes(value ^ mask[index % 4]
                            for index, value in enumerate(read_exact(connection, size)))
            if opcode & 15 == 8:
                break
            assert size < 126
            if opcode & 15 == 10:
                assert payload == b"gate"
                print("pong", flush=True)
                continue
            connection.sendall(bytes((opcode, size)) + payload)
            if payload == b"stale":
                connection.sendall(b"\x89\x04gate")
            print(payload.hex(), flush=True)
    except (EOFError, ConnectionError, ssl.SSLError):
        pass
    finally:
        connection.close()
        print("closed", flush=True)


tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
tls.load_cert_chain(sys.argv[1], sys.argv[2])
with socket.socket() as listener:
    listener.bind(("127.0.0.1", 0))
    listener.listen()
    print(listener.getsockname()[1], flush=True)
    while True:
        connection, address = listener.accept()
        threading.Thread(target=serve, args=(connection, tls), daemon=True).start()
