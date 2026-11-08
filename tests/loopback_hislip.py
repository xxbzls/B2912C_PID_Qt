"""Minimal loopback-only HiSLIP transport for IVI driver regression tests.

Implements the IVI-6.1 message subset needed by these tests, not a full server.
The responder handles SCPI; neither component accesses an external instrument.
"""
import socket
import struct
import threading

HEADER = struct.Struct('!2sBBIQ')


class HiSlipServer:
    def __init__(self, respond):
        self.respond = respond
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        self.errors = []
        self.trace = []
        self.sockets = []
        self.running = True
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        while self.running:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            self.sockets.append(conn)
            threading.Thread(target=self.serve, args=(conn,), daemon=True).start()

    @staticmethod
    def receive(conn, count):
        data = b''
        while len(data) < count:
            more = conn.recv(count-len(data))
            if not more:
                raise EOFError()
            data += more
        return data

    @staticmethod
    def send(conn, kind, control=0, parameter=0, payload=b''):
        conn.sendall(HEADER.pack(b'HS', kind, control, parameter, len(payload))+payload)

    def serve(self, conn):
        pending = b''
        try:
            with conn:
                while self.running:
                    tag, kind, control, parameter, size = HEADER.unpack(self.receive(conn, 16))
                    assert tag == b'HS'
                    data = self.receive(conn, size)
                    self.trace.append((kind, control, parameter, data))
                    print('HISLIP', kind, control, hex(parameter), repr(data), flush=True)
                    if kind == 0:  # Initialize -> InitializeResponse, synchronous mode.
                        self.send(conn, 1, parameter=0x01000001)
                    elif kind == 17:  # AsyncInitialize -> vendor ID KT.
                        self.send(conn, 18, parameter=0x4b54)
                    elif kind == 15:
                        self.send(conn, 16, payload=struct.pack('!Q', 1048576))
                    elif kind == 10:
                        self.send(conn, 11)
                    elif kind == 4:
                        self.send(conn, 5, control=1)
                    elif kind == 19:
                        self.send(conn, 23)
                    elif kind == 8:
                        pending = b''
                        self.send(conn, 9)
                    elif kind == 21:
                        self.send(conn, 22)
                    elif kind == 24:  # AsyncLockInfo: no exclusive/shared locks.
                        self.send(conn, 25)
                    elif kind in (6, 7):
                        pending += data
                        if kind == 7:
                            response = self.respond(pending)
                            pending = b''
                            if response is not None:
                                self.send(conn, 7, parameter=parameter, payload=response)
                    else:
                        raise RuntimeError(f'Unsupported test-server message {kind}')
        except (ConnectionResetError, EOFError, OSError):
            pass
        except Exception as exc:
            self.errors.append(exc)
            print('SERVER ERROR', repr(exc), flush=True)

    def close(self):
        self.running = False
        self.listener.close()
        for conn in self.sockets:
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
